// Settings persistence of the audio engine: versioned JSON (schema 1), atomic save, hostile-input safe load.
#include <algorithm>
#include <cctype>
#include <map>
#include <stdexcept>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

#include "audio_io/engine_impl.hpp"
#include "audio_io/json_util.hpp"

namespace hb::audio_io {

namespace json = hb::core::json;
using json_util::dbNum;
using json_util::num;
using json_util::quote;

namespace {
constexpr int kSchema = 1;
constexpr std::size_t kMaxFile = 4 * 1024 * 1024;

double getNum(const json::Value& o, const char* key, double def, double lo, double hi) {
    const json::Value* v = o.get(key);
    if (!v || !v->isNum() || !std::isfinite(v->n)) return def;
    return std::clamp(v->n, lo, hi);
}
bool getBool(const json::Value& o, const char* key, bool def) {
    const json::Value* v = o.get(key);
    if (!v) return def;
    if (v->isBool()) return v->b;
    return def;
}
std::string getStr(const json::Value& o, const char* key, std::size_t maxLen) {
    const json::Value* v = o.get(key);
    if (!v || !v->isStr() || v->s.size() > maxLen) return {};
    return v->s;
}
bool validTypeId(const std::string& s) {
    if (s.empty() || s.size() > 64) return false;
    for (unsigned char c : s)
        if (!(std::isalnum(c) || c == '.' || c == '_' || c == '-')) return false;
    return true;
}
bool validParamId(const std::string& s) {
    if (s.empty() || s.size() > 64) return false;
    for (unsigned char c : s)
        if (!(std::isalnum(c) || c == '_' || c == '.' || c == '-')) return false;
    return true;
}

void rename(std::map<std::string, json::Value>& o, const char* from, const char* to) {
    auto it = o.find(from);
    if (it == o.end() || o.count(to)) return;
    o[to] = std::move(it->second);
    o.erase(from);
}

// schema 0 = the unversioned prototype format (short key names).
void migrateV0(json::Value& root) {
    auto* strips = root.o.count("strips") ? &root.o["strips"] : nullptr;
    if (!strips || !strips->isArr()) return;
    for (auto& s : strips->a) {
        if (!s.isObj()) continue;
        rename(s.o, "trim", "trimDb");
        rename(s.o, "fader", "faderDb");
        rename(s.o, "phase", "phaseInvert");
        rename(s.o, "delay", "delayMs");
        rename(s.o, "toProgram", "sendToProgram");
        rename(s.o, "toMonitor", "monitor");
        if (s.o.count("device") && !s.o.count("input")) {
            json::Value in;
            in.t = json::Value::T::Obj;
            in.o["kind"].t = json::Value::T::Str;
            in.o["kind"].s = "device";
            in.o["id"] = s.o["device"];
            s.o["input"] = std::move(in);
            s.o.erase("device");
        }
        if (auto it = s.o.find("effects"); it != s.o.end() && it->second.isArr())
            for (auto& e : it->second.a)
                if (e.isObj()) rename(e.o, "typeId", "type");
    }
    rename(root.o, "output", "master");
}

void addMissingSlot(AudioEngine::Impl& P, int i, const std::string& type, bool bypass,
                    std::vector<std::pair<std::string, float>> saved) {
    std::lock_guard lock(P.ctl);
    StripCtl& c = P.strips[static_cast<std::size_t>(i)];
    if (c.chain->slots.size() >= static_cast<std::size_t>(kMaxEffects)) return;
    auto slot = std::make_shared<Slot>();
    slot->typeId = type;
    slot->name = type;
    slot->missing = true;
    slot->bypass.store(bypass);
    slot->saved = std::move(saved);
    auto chain = std::make_shared<Chain>(*c.chain);
    chain->slots.push_back(std::move(slot));
    P.publishChain(i, std::move(chain));
    P.version.fetch_add(1);
}
}  // namespace

std::string AudioEngine::settingsJson() const {
    Impl& P = *p_;
    std::lock_guard lock(P.ctl);
    std::string o = "{\"schema\":" + std::to_string(kSchema) + ",\"strips\":[";
    for (int i = 0; i < P.n; ++i) {
        const StripCtl& c = P.strips[static_cast<std::size_t>(i)];
        if (i) o += ',';
        o += "{\"name\":" + quote(c.name) + ",\"input\":{\"kind\":\"" + inputKindName(c.kind) + "\",\"id\":" +
             quote(c.kind == InputKind::Device ? c.deviceId : c.kind == InputKind::External ? c.externalId : "") +
             "},\"trimDb\":";
        num(o, c.trimDb);
        o += ",\"faderDb\":";
        dbNum(o, c.faderDb, 100.0f);
        o += std::string(",\"mute\":") + (c.mute ? "true" : "false") + ",\"solo\":" + (c.solo ? "true" : "false") +
             ",\"pan\":";
        num(o, c.pan);
        o += std::string(",\"phaseInvert\":") + (c.phase ? "true" : "false") + ",\"mono\":" + (c.mono ? "true" : "false") +
             ",\"delayMs\":";
        num(o, c.delayMs);
        o += std::string(",\"quality\":\"") + qualityName(c.quality) + "\",\"sendToProgram\":" +
             (c.toProgram ? "true" : "false") + ",\"monitor\":" + (c.toMonitor ? "true" : "false") + ",\"effects\":[";
        bool first = true;
        for (const auto& slot : c.chain->slots) {
            if (!first) o += ',';
            first = false;
            o += "{\"type\":" + quote(slot->typeId) + ",\"bypass\":" + (slot->bypass.load() ? "true" : "false") +
                 ",\"params\":{";
            bool fp = true;
            auto emit = [&](const std::string& id, float v) {
                if (!fp) o += ',';
                fp = false;
                o += quote(id) + ":";
                num(o, v);
            };
            if (slot->fx) {
                for (const auto& pi : slot->fx->params()) emit(pi.id, slot->fx->getParam(pi.id));
            } else {
                for (const auto& [id, v] : slot->saved) emit(id, v);
            }
            o += "}}";
        }
        o += "]}";
    }
    o += "],\"master\":{\"faderDb\":";
    dbNum(o, P.master.faderDb, 100.0f);
    o += std::string(",\"limiter\":") + (P.master.limiter ? "true" : "false") + ",\"ceilingDb\":";
    num(o, P.master.ceilingDb);
    o += "},\"monitor\":{\"deviceId\":" + quote(P.mon.deviceId) + ",\"level\":";
    num(o, P.mon.level);
    o += std::string(",\"mute\":") + (P.mon.mute ? "true" : "false") + "}}";
    return o;
}

bool AudioEngine::applySettingsJson(const std::string& text) {
    Impl& P = *p_;
    {
        std::lock_guard wl(P.warnMutex);
        P.warnings.clear();
    }
    auto warn = [&](const std::string& w) {
        std::lock_guard wl(P.warnMutex);
        if (P.warnings.size() < 16) P.warnings.push_back(w);
    };
    json::Value root;
    if (text.size() > kMaxFile || !json::parse(text, root) || !root.isObj()) {
        std::lock_guard el(P.errMutex);
        P.lastError = "Arquivo de configuração de áudio inválido ou corrompido; as configurações atuais foram mantidas.";
        return false;
    }
    int schema = 0;
    if (const json::Value* sv = root.get("schema"); sv && sv->isNum() && std::isfinite(sv->n))
        schema = static_cast<int>(std::clamp(sv->n, 0.0, 1e6));
    if (schema < 1) {
        migrateV0(root);
        warn("Configuração antiga (sem versão) migrada para o schema 1.");
    } else if (schema > kSchema) {
        warn("Configuração de uma versão mais nova (schema " + std::to_string(schema) +
             "); campos desconhecidos foram ignorados.");
    }

    const json::Value* strips = root.get("strips");
    static const json::Value kEmpty = [] {
        json::Value v;
        v.t = json::Value::T::Obj;
        return v;
    }();
    for (int i = 0; i < P.n; ++i) {
        const json::Value* so = strips && strips->isArr() && static_cast<std::size_t>(i) < strips->a.size() &&
                                        strips->a[static_cast<std::size_t>(i)].isObj()
                                    ? &strips->a[static_cast<std::size_t>(i)]
                                    : &kEmpty;
        const json::Value& s = *so;
        std::string name = sanitizeName(getStr(s, "name", 200), 32);
        setStripParam(i, "name", name.empty() ? std::string(defaultStripName(i)) : name);
        setStripParam(i, "trimDb", getNum(s, "trimDb", 0, -60, 24));
        const json::Value* fv = s.get("faderDb");
        setStripParam(i, "faderDb", fv && fv->isNum() && std::isfinite(fv->n) ? std::clamp(fv->n, -1000.0, 10.0) : 0.0);
        setStripParam(i, "mute", getBool(s, "mute", false));
        setStripParam(i, "solo", getBool(s, "solo", false));
        setStripParam(i, "pan", getNum(s, "pan", 0, -1, 1));
        setStripParam(i, "phase", getBool(s, "phaseInvert", false));
        setStripParam(i, "mono", getBool(s, "mono", false));
        setStripParam(i, "delayMs", getNum(s, "delayMs", 0, 0, 2000));
        setStripParam(i, "toProgram", getBool(s, "sendToProgram", true));
        setStripParam(i, "toMonitor", getBool(s, "monitor", false));
        std::string q = getStr(s, "quality", 16);
        if (q != "linear" && q != "high") q = "medium";
        setStripQuality(i, q);

        std::string kind = "none", id;
        if (const json::Value* in = s.get("input"); in && in->isObj()) {
            kind = getStr(*in, "kind", 16);
            id = getStr(*in, "id", 600);
            if ((kind != "device" && kind != "external") || id.empty()) kind = "none";
        }
        if (!setStripInput(i, kind, id)) setStripInput(i, "none");

        while (true) {  // clear the chain, then rebuild it from the file
            {
                std::lock_guard lock(P.ctl);
                if (P.strips[static_cast<std::size_t>(i)].chain->slots.empty()) break;
            }
            if (!removeEffect(i, 0)) break;
        }
        const json::Value* fx = s.get("effects");
        if (fx && fx->isArr()) {
            int count = 0;
            for (const auto& e : fx->a) {
                if (count >= kMaxEffects) {
                    warn("Faixa " + std::to_string(i + 1) + ": efeitos além do limite de " + std::to_string(kMaxEffects) +
                         " foram ignorados.");
                    break;
                }
                if (!e.isObj()) continue;
                const std::string type = getStr(e, "type", 64);
                if (!validTypeId(type)) continue;
                const bool bypass = getBool(e, "bypass", false);
                const json::Value* pv = e.get("params");
                std::vector<std::pair<std::string, float>> params;
                if (pv && pv->isObj()) {
                    for (const auto& [pid, val] : pv->o) {
                        if (params.size() >= 64) break;
                        if (validParamId(pid) && val.isNum() && std::isfinite(val.n))
                            params.emplace_back(pid, static_cast<float>(std::clamp(val.n, -1e9, 1e9)));
                    }
                }
                bool added = false;
                if (hb::audio::EffectRegistry::instance().contains(type) && addEffect(i, type)) {
                    added = true;
                    int idx;
                    {
                        std::lock_guard lock(P.ctl);
                        idx = static_cast<int>(P.strips[static_cast<std::size_t>(i)].chain->slots.size()) - 1;
                    }
                    for (const auto& [pid, v] : params) setEffectParam(i, idx, pid, v);  // unknown ids ignored
                    if (bypass) setEffectBypass(i, idx, true);
                }
                if (!added) {
                    addMissingSlot(P, i, type, bypass, std::move(params));
                    warn("Efeito ausente na faixa " + std::to_string(i + 1) + ": " + type + " (mantido na configuração).");
                }
                ++count;
            }
        }
    }

    const json::Value* m = root.get("master");
    const json::Value& mo = m && m->isObj() ? *m : kEmpty;
    const json::Value* mf = mo.get("faderDb");
    setMasterParam("faderDb", mf && mf->isNum() && std::isfinite(mf->n) ? std::clamp(mf->n, -1000.0, 10.0) : 0.0);
    setMasterParam("limiter", getBool(mo, "limiter", true) ? 1.0 : 0.0);
    setMasterParam("ceilingDb", getNum(mo, "ceilingDb", -1, -12, 0));

    const json::Value* mn = root.get("monitor");
    const json::Value& mno = mn && mn->isObj() ? *mn : kEmpty;
    setMonitor("level", getNum(mno, "level", 0.75, 0, 1));
    setMonitor("mute", getBool(mno, "mute", false) ? 1.0 : 0.0);
    const std::string dev = getStr(mno, "deviceId", 600);
    if (!setMonitor("deviceId", dev)) setMonitor("deviceId", std::string());
    return true;
}

std::vector<std::string> AudioEngine::settingsWarnings() const {
    std::lock_guard lock(p_->warnMutex);
    return p_->warnings;
}

bool AudioEngine::saveSettings(const std::string& path) const {
    if (path.empty()) {
        std::lock_guard el(p_->errMutex);
        p_->lastError = "Caminho de arquivo vazio.";
        return false;
    }
    namespace fs = std::filesystem;
    const std::string tmp = path + ".tmp";
    std::error_code ec;
    try {
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            if (!f) throw std::runtime_error("open");
            f << settingsJson();
            f.flush();
            if (!f.good()) throw std::runtime_error("write");
        }
        fs::rename(tmp, path, ec);  // replaces atomically (POSIX rename / MoveFileEx on Windows)
        if (ec) {
            fs::remove(path, ec);
            fs::rename(tmp, path, ec);
        }
        if (ec) throw std::runtime_error("rename");
    } catch (...) {
        std::error_code ec2;
        fs::remove(tmp, ec2);
        std::lock_guard el(p_->errMutex);
        p_->lastError = "Não foi possível gravar o arquivo de configuração de áudio.";
        return false;
    }
    return true;
}

bool AudioEngine::loadSettings(const std::string& path) {
    std::string text;
    try {
        std::ifstream f(path, std::ios::binary);
        if (!f) {
            std::lock_guard el(p_->errMutex);
            p_->lastError = "Arquivo de configuração de áudio não encontrado.";
            return false;
        }
        std::ostringstream ss;
        char buf[8192];
        while (f.read(buf, sizeof(buf)) || f.gcount() > 0) {
            ss.write(buf, f.gcount());
            if (ss.tellp() > static_cast<std::streamoff>(kMaxFile)) {
                std::lock_guard el(p_->errMutex);
                p_->lastError = "Arquivo de configuração de áudio grande demais.";
                return false;
            }
        }
        text = ss.str();
    } catch (...) {
        std::lock_guard el(p_->errMutex);
        p_->lastError = "Não foi possível ler o arquivo de configuração de áudio.";
        return false;
    }
    return applySettingsJson(text);
}

}  // namespace hb::audio_io
