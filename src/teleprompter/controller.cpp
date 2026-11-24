#include "teleprompter/controller.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "core/json.hpp"

namespace fs = std::filesystem;
namespace json = hb::core::json;

namespace hb::teleprompter {

namespace {
// Splits "a|b|c|rest" into at most `maxParts` parts; the last keeps any further '|'.
std::vector<std::string> splitMsg(const std::string& msg, std::size_t maxParts) {
    std::vector<std::string> a;
    std::size_t s0 = 0;
    for (std::size_t i = 0; i <= msg.size(); ++i) {
        if (i == msg.size() || (msg[i] == '|' && a.size() + 1 < maxParts)) {
            a.push_back(msg.substr(s0, i - s0));
            s0 = i + 1;
        }
    }
    return a;
}

bool parseNum(const std::string& s, double& out) {
    if (s.empty() || s.size() > 32) return false;
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    if (end == s.c_str() || *end != '\0' || !std::isfinite(v)) return false;
    out = v;
    return true;
}

bool validColor(const std::string& c) {
    if (c.size() != 7 || c[0] != '#') return false;
    return std::all_of(c.begin() + 1, c.end(), [](unsigned char ch) { return std::isxdigit(ch) != 0; });
}

std::string fmt(const char* f, double v) {
    char b[48];
    std::snprintf(b, sizeof b, f, v);
    return b;
}

std::string q(const std::string& s) { return "\"" + json::escape(s) + "\""; }
const char* tf(bool b) { return b ? "true" : "false"; }

const char* kSample =
    "# Roteiro de exemplo - linhas que começam com # são anotações só para o operador (a pessoa à frente da câmera não as vê).\n"
    "# Dica: espaço = play/pause, setas ↑/↓ = velocidade, Home = voltar ao início.\n"
    "\n"
    "Olá, pessoal! Bem-vindos à nossa transmissão ao vivo.\n"
    "Hoje vamos falar sobre como usar o teleprompter do HB Studio para ler o texto olhando direto para a câmera.\n"
    "\n"
    "# [PAUSA] respirar e sorrir\n"
    "\n"
    "Parágrafos separados por uma linha em branco viram blocos que o operador pode clicar para pular direto até eles.\n"
    "Você controla a velocidade em palavras por minuto, o tamanho da letra, as cores e o espelhamento.\n"
    "\n"
    "# [CÂMERA 2] mostrar o produto\n"
    "\n"
    "Obrigado por assistir. Até a próxima!\n";
}  // namespace

Controller::Controller(fs::path dir, const Clock* clock)
    : clock_(clock), dir_(dir), store_(dir), scroll_(clock) {}

void Controller::load() {
    std::lock_guard lock(mu_);
    store_.ensureDir();
    std::string lastId;
    {
        std::ifstream f(dir_ / "settings.json", std::ios::binary);
        std::stringstream ss;
        ss << f.rdbuf();
        json::Value v;
        if (f && json::parse(ss.str(), v) && v.isObj()) {
            auto num = [&](const char* k, double lo, double hi, double& dst) {
                const json::Value* x = v.get(k);
                if (x && x->isNum() && std::isfinite(x->n)) dst = std::clamp(x->n, lo, hi);
            };
            auto str = [&](const char* k, std::string& dst) {
                const json::Value* x = v.get(k);
                if (x && x->isStr()) dst = x->s;
            };
            auto flag = [&](const char* k, bool& dst) {
                const json::Value* x = v.get(k);
                if (x && x->isBool()) dst = x->b;
            };
            double d;
            d = style_.fontPx; num("font", 20, 220, d); style_.fontPx = static_cast<int>(d);
            num("lh", 1.0, 2.5, style_.lineHeight);
            d = style_.marginPct; num("margin", 0, 30, d); style_.marginPct = static_cast<int>(d);
            std::string c;
            c = style_.fg; str("fg", c); if (validColor(c)) style_.fg = c;
            c = style_.bg; str("bg", c); if (validColor(c)) style_.bg = c;
            flag("mirror", style_.mirror);
            flag("flip", style_.flip);
            flag("marker", style_.marker);
            d = style_.markerPct; num("markerpos", 10, 80, d); style_.markerPct = static_cast<int>(d);
            std::string mode;
            str("mode", mode);
            if (mode == "pxs") scroll_.setMode(SpeedMode::PxPerSec);
            double w = scroll_.wpm(), p = scroll_.pxPerSec(), cd = scroll_.countdown();
            num("wpm", kMinWpm, kMaxWpm, w);
            num("pxs", kMinPxPerSec, kMaxPxPerSec, p);
            num("countdown", 0, kMaxCountdownSec, cd);
            scroll_.setWpm(w);
            scroll_.setPxPerSec(p);
            scroll_.setCountdown(static_cast<int>(cd));
            str("script", lastId);
        }
    }
    if (store_.list().empty()) store_.create("Roteiro de exemplo", kSample);
    if (lastId.empty() || !store_.exists(lastId)) {
        const auto l = store_.list();
        lastId = l.empty() ? std::string() : l.front().id;
    }
    if (!lastId.empty()) selectLocked(lastId);
}

bool Controller::selectLocked(const std::string& id) {
    std::string text;
    if (!isValidId(id) || !store_.load(id, text)) return false;
    currentId_ = id;
    currentName_ = store_.nameOf(id);
    currentText_ = std::move(text);
    parsed_ = parseScript(currentText_);
    scroll_.setScriptWords(parsed_.totalWords);
    scroll_.restart();
    markSettingsDirty();
    return true;
}

void Controller::markSettingsDirty() {
    settingsDirty_ = true;
    settingsDirtyAt_ = clock_->now();
}

void Controller::saveSettingsLocked() {
    settingsDirty_ = false;
    json::Value v;
    v.t = json::Value::T::Obj;
    auto num = [&](const char* k, double n) { json::Value x; x.t = json::Value::T::Num; x.n = n; v.o[k] = x; };
    auto str = [&](const char* k, const std::string& s) { json::Value x; x.t = json::Value::T::Str; x.s = s; v.o[k] = x; };
    auto flag = [&](const char* k, bool b) { json::Value x; x.t = json::Value::T::Bool; x.b = b; v.o[k] = x; };
    num("font", style_.fontPx);
    num("lh", style_.lineHeight);
    num("margin", style_.marginPct);
    str("fg", style_.fg);
    str("bg", style_.bg);
    flag("mirror", style_.mirror);
    flag("flip", style_.flip);
    flag("marker", style_.marker);
    num("markerpos", style_.markerPct);
    str("mode", speedModeName(scroll_.mode()));
    num("wpm", scroll_.wpm());
    num("pxs", scroll_.pxPerSec());
    num("countdown", scroll_.countdown());
    str("script", currentId_);
    const fs::path p = dir_ / "settings.json";
    fs::path tmp = p;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return;
        f << json::dump(v, true);
    }
    std::error_code ec;
    fs::rename(tmp, p, ec);
    if (ec) {
        fs::remove(p, ec);
        ec.clear();
        fs::rename(tmp, p, ec);
    }
}

void Controller::flush() {
    std::lock_guard lock(mu_);
    if (settingsDirty_) saveSettingsLocked();
}

Effects Controller::execute(const std::string& message) {
    std::lock_guard lock(mu_);
    try {
        return executeLocked(message);
    } catch (...) {   // a bad command must never take the host down
        Effects e;
        e.handled = true;
        e.toast = "Teleprompter: comando inválido.";
        return e;
    }
}

Effects Controller::executeLocked(const std::string& message) {
    Effects fx;
    if (message.compare(0, 3, "tp|") != 0 && message != "tp") return fx;
    fx.handled = true;
    const auto a = splitMsg(message, 4);
    if (a.size() < 2) return fx;
    const std::string& c = a[1];
    auto arg = [&](std::size_t i) -> const std::string& {
        static const std::string empty;
        return i < a.size() ? a[i] : empty;
    };
    double n = 0;

    if (c == "hello") {
        fx.state = fx.list = fx.script = fx.style = true;
    } else if (c == "play") { scroll_.play(); fx.state = true; }
    else if (c == "pause") { scroll_.pause(); fx.state = true; }
    else if (c == "toggle") { scroll_.toggle(); fx.state = true; }
    else if (c == "restart") { scroll_.restart(); fx.state = true; }
    else if (c == "speed" && parseNum(arg(2), n)) { scroll_.setSpeed(n); markSettingsDirty(); fx.state = true; }
    else if (c == "nudge" && parseNum(arg(2), n)) { scroll_.nudgeSpeed(n); markSettingsDirty(); fx.state = true; }
    else if (c == "mode" && (arg(2) == "wpm" || arg(2) == "pxs")) {
        scroll_.setMode(arg(2) == "wpm" ? SpeedMode::Wpm : SpeedMode::PxPerSec);
        markSettingsDirty();
        fx.state = true;
    } else if (c == "seek" && parseNum(arg(2), n)) { scroll_.seek(n); fx.state = true; }
    else if (c == "countdown" && parseNum(arg(2), n)) {
        scroll_.setCountdown(static_cast<int>(std::lround(std::clamp(n, 0.0, static_cast<double>(kMaxCountdownSec)))));
        markSettingsDirty();
        fx.state = true;
    } else if (c == "layout") {
        double content = 0, view = 0;
        const std::string& src = arg(2);
        // arg(3) carries "content|viewport" (the 4-part split keeps the remainder in the last part).
        const std::string& rest = arg(3);
        const auto bar = rest.find('|');
        if ((src == "p" || src == "o") && bar != std::string::npos && parseNum(rest.substr(0, bar), content) &&
            parseNum(rest.substr(bar + 1), view)) {
            if (src == "p") {
                presenterLayout_ = true;
                scroll_.setLayout(content, view);
            } else if (!(presenterOpen_ && presenterLayout_)) {
                scroll_.setLayout(content, view);
            }
            // Only the derived px/s <-> wpm display changes; views are not told to re-render for it.
        }
    } else if (c == "select") {
        if (selectLocked(arg(2))) fx.state = fx.script = fx.list = true;
        else fx.toast = "Roteiro não encontrado.";
    } else if (c == "new") {
        const std::string id = store_.create(arg(2));
        if (id.empty()) fx.toast = "Não foi possível criar o roteiro (limite de " + std::to_string(kMaxScripts) + " ou erro de disco).";
        else { selectLocked(id); fx.state = fx.script = fx.list = true; }
    } else if (c == "rename") {
        if (store_.rename(arg(2), arg(3))) {
            if (arg(2) == currentId_) currentName_ = store_.nameOf(currentId_);
            fx.list = fx.script = true;
        } else fx.toast = "Não foi possível renomear o roteiro.";
    } else if (c == "delete") {
        if (store_.remove(arg(2))) {
            if (arg(2) == currentId_) {
                const auto l = store_.list();
                std::string next = l.empty() ? store_.create("Novo roteiro") : l.front().id;
                currentId_.clear();
                selectLocked(next);
            }
            fx.state = fx.script = fx.list = true;
            fx.toast = "Roteiro excluído.";
        } else fx.toast = "Não foi possível excluir o roteiro.";
    } else if (c == "save") {
        const std::string text = normalizeText(arg(3));
        if (store_.save(arg(2), text)) {
            if (arg(2) == currentId_) {
                currentText_ = text;
                parsed_ = parseScript(currentText_);
                scroll_.setScriptWords(parsed_.totalWords);
                fx.script = fx.state = true;
            }
        } else fx.toast = "Não foi possível salvar o roteiro.";
    } else if (c == "import") {
        const std::string id = store_.importText(arg(2), arg(3));
        if (id.empty()) fx.toast = "Não foi possível importar o arquivo.";
        else { selectLocked(id); fx.state = fx.script = fx.list = true; fx.toast = "Roteiro importado."; }
    } else if (c == "style" && !arg(2).empty()) {
        const std::string& k = arg(2);
        const std::string& val = arg(3);
        bool ok = true;
        if (k == "font" && parseNum(val, n)) style_.fontPx = static_cast<int>(std::lround(std::clamp(n, 20.0, 220.0)));
        else if (k == "lh" && parseNum(val, n)) style_.lineHeight = std::clamp(n, 1.0, 2.5);
        else if (k == "margin" && parseNum(val, n)) style_.marginPct = static_cast<int>(std::lround(std::clamp(n, 0.0, 30.0)));
        else if (k == "markerpos" && parseNum(val, n)) style_.markerPct = static_cast<int>(std::lround(std::clamp(n, 10.0, 80.0)));
        else if (k == "fg" && validColor(val)) style_.fg = val;
        else if (k == "bg" && validColor(val)) style_.bg = val;
        else if (k == "mirror" && (val == "0" || val == "1")) style_.mirror = val == "1";
        else if (k == "flip" && (val == "0" || val == "1")) style_.flip = val == "1";
        else if (k == "marker" && (val == "0" || val == "1")) style_.marker = val == "1";
        else ok = false;
        if (ok) { markSettingsDirty(); fx.style = true; }
    } else if (c == "presenter") {
        if (arg(2) == "open") fx.presenter = Effects::Presenter::Open;
        else if (arg(2) == "close") fx.presenter = Effects::Presenter::Close;
        else if (arg(2) == "fullscreen") fx.presenter = Effects::Presenter::Fullscreen;
    }
    return fx;
}

void Controller::setPresenterOpen(bool open) {
    std::lock_guard lock(mu_);
    if (presenterOpen_ == open) return;
    presenterOpen_ = open;
    if (!open) presenterLayout_ = false;
    lastSentAt_ = -1e9;   // forces the next poll() to publish the new flag
}

std::optional<std::string> Controller::poll() {
    std::lock_guard lock(mu_);
    const double now = clock_->now();
    if (settingsDirty_ && now - settingsDirtyAt_ > 1.0) saveSettingsLocked();
    const Snapshot s = scroll_.snapshot();
    const bool moving = s.state == RunState::Playing || s.state == RunState::Countdown;
    if (s.state != lastSentState_ || now - lastSentAt_ >= (moving ? kResyncSeconds : 5.0))
        return stateJsonLocked();
    return std::nullopt;
}

Snapshot Controller::snapshot() {
    std::lock_guard lock(mu_);
    return scroll_.snapshot();
}

Style Controller::style() {
    std::lock_guard lock(mu_);
    return style_;
}

std::string Controller::stateJson() {
    std::lock_guard lock(mu_);
    return stateJsonLocked();
}

std::string Controller::stateJsonLocked() {
    const Snapshot s = scroll_.snapshot();
    lastSentState_ = s.state;
    lastSentAt_ = clock_->now();
    std::string j = "{\"t\":\"tp\",\"seq\":" + std::to_string(++seq_);
    j += ",\"state\":" + q(runStateName(s.state));
    j += ",\"pos\":" + fmt("%.6f", s.pos);
    j += ",\"rate\":" + fmt("%.9f", s.rate);
    j += ",\"cd\":" + fmt("%.2f", s.countdownLeft);
    j += ",\"cdSec\":" + std::to_string(s.countdownSec);
    j += ",\"remain\":" + fmt("%.1f", s.remainingSec);
    j += ",\"total\":" + fmt("%.1f", s.totalSec);
    j += ",\"mode\":" + q(speedModeName(s.mode));
    j += ",\"wpm\":" + fmt("%.1f", s.wpm);
    j += ",\"pxs\":" + fmt("%.1f", s.pxPerSec);
    j += ",\"words\":" + std::to_string(s.words);
    j += ",\"script\":" + q(currentId_);
    j += std::string(",\"presenter\":") + tf(presenterOpen_) + "}";
    return j;
}

std::string Controller::listJson() {
    std::lock_guard lock(mu_);
    std::string j = "{\"t\":\"tplist\",\"current\":" + q(currentId_) + ",\"scripts\":[";
    bool first = true;
    for (const auto& i : store_.list()) {
        j += std::string(first ? "" : ",") + "{\"id\":" + q(i.id) + ",\"name\":" + q(i.name) + ",\"words\":" +
             std::to_string(i.words) + "}";
        first = false;
    }
    return j + "]}";
}

std::string Controller::scriptJson() {
    std::lock_guard lock(mu_);
    std::string j = "{\"t\":\"tpscript\",\"id\":" + q(currentId_) + ",\"name\":" + q(currentName_) +
                    ",\"words\":" + std::to_string(parsed_.totalWords) + ",\"paragraphs\":" + std::to_string(parsed_.paragraphs) +
                    ",\"text\":" + q(currentText_) + ",\"blocks\":[";
    const auto starts = paragraphStarts(parsed_);
    std::size_t pi = 0;
    bool first = true;
    for (const auto& b : parsed_.blocks) {
        const bool para = b.kind == Block::Kind::Paragraph;
        j += std::string(first ? "" : ",") + "{\"k\":\"" + (para ? "p" : "c") + "\",\"t\":" + q(b.text) +
             ",\"w\":" + std::to_string(b.words) + ",\"line\":" + std::to_string(b.line);
        if (para) j += ",\"at\":" + fmt("%.6f", starts[pi++]);
        j += "}";
        first = false;
    }
    return j + "]}";
}

std::string Controller::styleJson() {
    std::lock_guard lock(mu_);
    std::string j = "{\"t\":\"tpstyle\",\"font\":" + std::to_string(style_.fontPx) + ",\"lh\":" + fmt("%.2f", style_.lineHeight) +
                    ",\"margin\":" + std::to_string(style_.marginPct) + ",\"fg\":" + q(style_.fg) + ",\"bg\":" + q(style_.bg) +
                    ",\"mirror\":" + tf(style_.mirror) + ",\"flip\":" + tf(style_.flip) + ",\"marker\":" + tf(style_.marker) +
                    ",\"markerpos\":" + std::to_string(style_.markerPct) + ",\"cdSec\":" + std::to_string(scroll_.countdown()) + "}";
    return j;
}

}  // namespace hb::teleprompter
