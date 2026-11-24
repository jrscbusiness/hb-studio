#include "ai/provider_store.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>

#include "ai/protocol.hpp"
#include "core/json.hpp"

namespace hb::ai {

namespace {

using Json = hb::core::json::Value;

std::string esc(const std::string& s) { return hb::core::json::escape(s); }

std::string cleanText(const std::string& in, std::size_t maxLen) {
    std::string o;
    for (unsigned char c : in) {
        if (c < 0x20 || c == 0x7F || c == '<' || c == '>') continue;
        if (o.size() >= maxLen) break;
        o += static_cast<char>(c);
    }
    std::size_t a = 0, b = o.size();
    while (a < b && o[a] == ' ') ++a;
    while (b > a && o[b - 1] == ' ') --b;
    return o.substr(a, b - a);
}

// Keys are pasted from web pages: drop every whitespace/control character (a trailing newline is the
// classic cause of "invalid key" errors). Real keys contain none.
std::string cleanKey(const std::string& in) {
    std::string o;
    for (unsigned char c : in) {
        if (c <= 0x20 || c == 0x7F) continue;
        if (o.size() >= 512) break;
        o += static_cast<char>(c);
    }
    return o;
}

std::string cleanModel(const std::string& in) {
    std::string o;
    for (unsigned char c : in) {
        if (c <= 0x20 || c == 0x7F || c == '<' || c == '>' || c == '"' || c == '\\') continue;
        if (o.size() >= 200) break;
        o += static_cast<char>(c);
    }
    return o;
}

void readProvider(const Json& o, Provider& p, bool* hasKey) {
    auto str = [&](const char* k, std::string& dst) {
        if (const Json* v = o.get(k); v && v->isStr()) dst = v->s;
    };
    str("id", p.id);
    str("name", p.name);
    str("baseUrl", p.baseUrl);
    str("model", p.model);
    if (const Json* v = o.get("kind"); v && v->isStr()) parseKind(v->s, p.kind);
    if (const Json* v = o.get("apiKey"); v && v->isStr()) {
        p.apiKey = v->s;
        if (hasKey) *hasKey = !cleanKey(v->s).empty();
    }
}

}  // namespace

std::vector<Provider> ProviderStore::list() const {
    std::lock_guard lock(mutex_);
    return p_;
}

Provider ProviderStore::get(const std::string& id) const {
    if (id.empty()) return {};
    std::lock_guard lock(mutex_);
    for (const auto& p : p_)
        if (p.id == id) return p;
    return {};
}

Provider ProviderStore::active() const {
    std::lock_guard lock(mutex_);
    for (const auto& p : p_)
        if (p.id == active_) return p;
    return {};
}

std::string ProviderStore::activeId() const {
    std::lock_guard lock(mutex_);
    return active_;
}

std::string ProviderStore::upsert(Provider p, bool keepKey, std::string* err) {
    auto fail = [&](const char* m) {
        if (err) *err = m;
        return std::string();
    };
    p.name = cleanText(p.name, 64);
    if (p.name.empty()) p.name = defaultName(p.kind);
    if (p.baseUrl.empty()) p.baseUrl = defaultBaseUrl(p.kind);
    std::string norm, e;
    if (!normalizeBaseUrl(p.baseUrl, norm, e)) {
        if (err) *err = e;
        return {};
    }
    p.baseUrl = norm;
    p.model = cleanModel(p.model);
    p.apiKey = cleanKey(p.apiKey);

    std::lock_guard lock(mutex_);
    if (!p.id.empty()) {
        for (auto& e2 : p_) {
            if (e2.id != p.id) continue;
            if (keepKey) p.apiKey = e2.apiKey;
            e2 = p;
            return p.id;
        }
        return fail("Provedor não encontrado.");
    }
    if (p_.size() >= static_cast<std::size_t>(kMax)) return fail("Limite de 16 provedores atingido. Remova algum antes de adicionar outro.");
    p.id = "p" + std::to_string(seq_++);
    if (p_.empty()) active_ = p.id;
    p_.push_back(p);
    return p.id;
}

bool ProviderStore::remove(const std::string& id) {
    std::lock_guard lock(mutex_);
    const auto before = p_.size();
    p_.erase(std::remove_if(p_.begin(), p_.end(), [&](const Provider& p) { return p.id == id; }), p_.end());
    if (p_.size() == before) return false;
    if (active_ == id) active_ = p_.empty() ? "" : p_.front().id;
    return true;
}

bool ProviderStore::activate(const std::string& id) {
    std::lock_guard lock(mutex_);
    for (const auto& p : p_)
        if (p.id == id) { active_ = id; return true; }
    return false;
}

std::string ProviderStore::toJson() const {
    std::lock_guard lock(mutex_);
    std::ostringstream o;
    o << "{\"schema\":1,\"nextSeq\":" << seq_ << ",\"active\":\"" << esc(active_) << "\",\"providers\":[";
    for (std::size_t i = 0; i < p_.size(); ++i) {
        const Provider& p = p_[i];
        if (i) o << ",";
        o << "{\"id\":\"" << esc(p.id) << "\",\"name\":\"" << esc(p.name) << "\",\"kind\":\"" << kindId(p.kind)
          << "\",\"baseUrl\":\"" << esc(p.baseUrl) << "\",\"model\":\"" << esc(p.model) << "\"";
        std::string blob;
        if (secrets_ && !p.apiKey.empty() && secrets_->protect(p.apiKey, blob)) o << ",\"keyBlob\":\"" << toHex(blob) << "\"";
        o << "}";
    }
    o << "]}";
    return o.str();
}

bool ProviderStore::fromJson(const std::string& json) {
    Json root;
    if (!hb::core::json::parse(json, root) || !root.isObj()) return false;
    const Json* arr = root.get("providers");
    if (!arr || !arr->isArr()) return false;
    std::vector<Provider> n;
    bool lost = false;
    for (const auto& j : arr->a) {
        if (n.size() >= static_cast<std::size_t>(kMax)) break;
        if (!j.isObj()) continue;
        Provider p;
        readProvider(j, p, nullptr);
        p.apiKey.clear();  // never trust a plaintext key field in the file
        std::string norm, e;
        if (p.id.empty() || !normalizeBaseUrl(p.baseUrl, norm, e)) continue;
        p.baseUrl = norm;
        p.name = cleanText(p.name, 64);
        if (p.name.empty()) p.name = defaultName(p.kind);
        p.model = cleanModel(p.model);
        if (const Json* kb = j.get("keyBlob"); kb && kb->isStr()) {
            std::string blob, plain;
            if (secrets_ && fromHex(kb->s, blob) && secrets_->unprotect(blob, plain)) p.apiKey = cleanKey(plain);
            else lost = true;
        }
        n.push_back(std::move(p));
    }
    std::string act;
    if (const Json* a = root.get("active"); a && a->isStr()) act = a->s;
    unsigned seq = 1;
    if (const Json* s = root.get("nextSeq"); s && s->isNum() && s->n >= 1 && s->n < 1e9) seq = static_cast<unsigned>(s->n);
    std::lock_guard lock(mutex_);
    p_ = std::move(n);
    active_.clear();
    for (const auto& p : p_)
        if (p.id == act) active_ = act;
    if (active_.empty() && !p_.empty()) active_ = p_.front().id;
    seq_ = seq;
    lostKeys_ = lost;
    return true;
}

bool ProviderStore::load(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    if (ss.str().size() > 1024 * 1024) return false;
    return fromJson(ss.str());
}

bool ProviderStore::save(const std::string& path) const {
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << toJson();
        if (!f.good()) return false;
    }
    std::remove(path.c_str());
    return std::rename(tmp.c_str(), path.c_str()) == 0;
}

bool ProviderStore::lastLoadLostKeys() const {
    std::lock_guard lock(mutex_);
    return lostKeys_;
}

std::string ProviderStore::uiJson() const {
    std::lock_guard lock(mutex_);
    std::ostringstream o;
    bool configured = false;
    for (const auto& p : p_)
        if (p.id == active_ && !p.model.empty() && (!p.keyRequired() || !p.apiKey.empty())) configured = true;
    o << "{\"t\":\"ai_state\",\"active\":\"" << esc(active_) << "\",\"configured\":" << (configured ? "true" : "false")
      << ",\"canPersistKeys\":" << (secrets_ ? "true" : "false") << ",\"keysLost\":" << (lostKeys_ ? "true" : "false")
      << ",\"list\":[";
    for (std::size_t i = 0; i < p_.size(); ++i) {
        const Provider& p = p_[i];
        if (i) o << ",";
        o << "{\"id\":\"" << esc(p.id) << "\",\"name\":\"" << esc(p.name) << "\",\"kind\":\"" << kindId(p.kind)
          << "\",\"baseUrl\":\"" << esc(p.baseUrl) << "\",\"model\":\"" << esc(p.model) << "\",\"keySet\":"
          << (p.apiKey.empty() ? "false" : "true") << ",\"keyHint\":\"" << esc(maskKey(p.apiKey)) << "\"}";
    }
    o << "]}";
    return o.str();
}

bool ProviderStore::parseProvider(const std::string& json, Provider& out, bool& hasKey) {
    Json root;
    hasKey = false;
    if (!hb::core::json::parse(json, root) || !root.isObj()) return false;
    Provider p;
    readProvider(root, p, &hasKey);
    out = std::move(p);
    return true;
}

}  // namespace hb::ai
