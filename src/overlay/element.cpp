#include "overlay/element.hpp"

#include <algorithm>
#include <cmath>

#include "qr/qr.hpp"

namespace hb::overlay {

namespace {

using Json = core::json::Value;

const char* const kTypes[] = {"timer", "scoreboard", "qr", "lowerthird", "captions", "chatpin"};

bool oneOf(const std::string& v, std::initializer_list<const char*> opts) {
    for (const char* o : opts) if (v == o) return true;
    return false;
}

std::string clip(const std::string& s, std::size_t maxBytes) { return sanitizeUtf8(s, maxBytes); }

int roundi(double v) { return static_cast<int>(std::lround(v)); }

}  // namespace

bool isValidType(const std::string& type) {
    for (const char* t : kTypes) if (type == t) return true;
    return false;
}

ElementConfig defaultConfig(const std::string& type) {
    ElementConfig c;
    c.type = isValidType(type) ? type : "timer";
    if (c.type == "timer") {
        c.name = "Cronômetro"; c.x = 0.97; c.y = 0.05; c.pivotX = 1; c.pivotY = 0; c.size = 0.07;
    } else if (c.type == "scoreboard") {
        c.name = "Placar"; c.x = 0.5; c.y = 0.04; c.pivotX = 0.5; c.pivotY = 0; c.size = 0.045; c.animIn = "slide-down"; c.animOut = "slide-down";
    } else if (c.type == "qr") {
        c.name = "QR Code"; c.x = 0.97; c.y = 0.95; c.pivotX = 1; c.pivotY = 1; c.size = 0.03; c.qrCaption = "Apoie esta transmissão";
    } else if (c.type == "lowerthird") {
        c.name = "Nome (lower third)"; c.x = 0.04; c.y = 0.88; c.pivotX = 0; c.pivotY = 1; c.size = 0.05;
        c.animIn = "slide-left"; c.animOut = "slide-left"; c.title = "Nome Sobrenome"; c.subtitle = "Função / Cidade";
    } else if (c.type == "captions") {
        c.name = "Legendas ao vivo"; c.x = 0.5; c.y = 0.95; c.pivotX = 0.5; c.pivotY = 1; c.size = 0.05; c.bold = false;
        c.holdSec = 4; c.maxLines = 2; c.widthPct = 0.8; c.animMs = 200; c.bgAlpha = 0.7;
    } else {  // chatpin
        c.name = "Chat destacado"; c.x = 0.04; c.y = 0.75; c.pivotX = 0; c.pivotY = 1; c.size = 0.035; c.bold = false;
        c.holdSec = 12; c.maxLines = 4; c.widthPct = 0.35; c.animIn = "slide-left"; c.animOut = "fade"; c.bgAlpha = 0.8;
    }
    return c;
}

void sanitize(ElementConfig& c) {
    if (!isValidType(c.type)) c.type = "timer";
    c.name = clip(c.name, 64);
    c.id = clip(c.id, 40);
    c.font = clip(c.font, 64);
    if (c.font.empty()) c.font = "Segoe UI";
    auto fin = [](double v, double def) { return std::isfinite(v) ? v : def; };
    c.x = std::clamp(fin(c.x, 0.5), -0.5, 1.5); c.y = std::clamp(fin(c.y, 0.5), -0.5, 1.5);
    c.pivotX = std::clamp(fin(c.pivotX, 0.5), 0.0, 1.0); c.pivotY = std::clamp(fin(c.pivotY, 0.5), 0.0, 1.0);
    c.z = std::clamp(c.z, -1000, 1000);
    c.size = std::clamp(fin(c.size, 0.06), 0.012, 0.3);
    c.textColor &= 0xFFFFFF; c.bgColor &= 0xFFFFFF; c.accent &= 0xFFFFFF; c.colorA &= 0xFFFFFF; c.colorB &= 0xFFFFFF;
    c.qrFg &= 0xFFFFFF; c.qrBg &= 0xFFFFFF;
    c.bgAlpha = std::clamp(fin(c.bgAlpha, 0.6), 0.0, 1.0);
    c.opacity = std::clamp(fin(c.opacity, 1.0), 0.0, 1.0);
    const auto anims = {"none", "fade", "slide-left", "slide-right", "slide-up", "slide-down"};
    if (!oneOf(c.animIn, anims)) c.animIn = "fade";
    if (!oneOf(c.animOut, anims)) c.animOut = "fade";
    c.animMs = std::clamp(c.animMs, 0, 5000);
    if (!oneOf(c.mode, {"down", "up"})) c.mode = "down";
    c.durationSec = std::clamp(c.durationSec, 0, 359999);
    if (!oneOf(c.format, {"auto", "mm:ss", "hh:mm:ss"})) c.format = "auto";
    c.label = clip(c.label, 80); c.teamA = clip(c.teamA, 40); c.teamB = clip(c.teamB, 40); c.period = clip(c.period, 40);
    c.scoreA = std::clamp(c.scoreA, 0, 999); c.scoreB = std::clamp(c.scoreB, 0, 999);
    c.qrText = clip(c.qrText, 600); c.qrCaption = clip(c.qrCaption, 120);
    if (!oneOf(c.qrEcc, {"L", "M", "Q", "H"})) c.qrEcc = "M";
    c.qrSize = std::clamp(fin(c.qrSize, 0.28), 0.1, 0.8);
    c.title = clip(c.title, 120); c.subtitle = clip(c.subtitle, 160);
    c.holdSec = std::clamp(fin(c.holdSec, 4.0), 0.0, 3600.0);
    c.maxLines = std::clamp(c.maxLines, 1, 6);
    c.widthPct = std::clamp(fin(c.widthPct, 0.8), 0.1, 1.0);
}

// ---------------- JSON ----------------
namespace {
Json jstr(const std::string& s) { Json v; v.t = Json::T::Str; v.s = s; return v; }
Json jnum(double n) { Json v; v.t = Json::T::Num; v.n = n; return v; }
Json jbool(bool b) { Json v; v.t = Json::T::Bool; v.b = b; return v; }

void getStr(const Json& o, const char* k, std::string& dst) { if (auto* p = o.get(k); p && p->isStr()) dst = p->s; }
void getNum(const Json& o, const char* k, double& dst) { if (auto* p = o.get(k); p && p->isNum()) dst = p->n; }
void getInt(const Json& o, const char* k, int& dst) {
    if (auto* p = o.get(k); p && p->isNum() && std::isfinite(p->n)) dst = static_cast<int>(std::clamp(p->n, -1e9, 1e9));
}
void getBool(const Json& o, const char* k, bool& dst) { if (auto* p = o.get(k); p && p->isBool()) dst = p->b; }
void getCol(const Json& o, const char* k, std::uint32_t& dst) {
    if (auto* p = o.get(k); p && p->isNum() && std::isfinite(p->n)) dst = static_cast<std::uint32_t>(std::clamp(p->n, 0.0, 16777215.0));
}
}  // namespace

Json toJson(const ElementConfig& c) {
    Json o; o.t = Json::T::Obj;
    auto& m = o.o;
    m["id"] = jstr(c.id); m["type"] = jstr(c.type); m["name"] = jstr(c.name);
    m["x"] = jnum(c.x); m["y"] = jnum(c.y); m["pivotX"] = jnum(c.pivotX); m["pivotY"] = jnum(c.pivotY); m["z"] = jnum(c.z);
    m["size"] = jnum(c.size); m["font"] = jstr(c.font); m["bold"] = jbool(c.bold);
    m["textColor"] = jnum(c.textColor); m["bgColor"] = jnum(c.bgColor); m["accent"] = jnum(c.accent);
    m["bgAlpha"] = jnum(c.bgAlpha); m["opacity"] = jnum(c.opacity);
    m["animIn"] = jstr(c.animIn); m["animOut"] = jstr(c.animOut); m["animMs"] = jnum(c.animMs);
    m["mode"] = jstr(c.mode); m["durationSec"] = jnum(c.durationSec); m["format"] = jstr(c.format);
    m["blinkAtZero"] = jbool(c.blinkAtZero); m["label"] = jstr(c.label);
    m["teamA"] = jstr(c.teamA); m["teamB"] = jstr(c.teamB); m["period"] = jstr(c.period);
    m["colorA"] = jnum(c.colorA); m["colorB"] = jnum(c.colorB); m["scoreA"] = jnum(c.scoreA); m["scoreB"] = jnum(c.scoreB);
    m["showClock"] = jbool(c.showClock);
    m["qrText"] = jstr(c.qrText); m["qrCaption"] = jstr(c.qrCaption); m["qrEcc"] = jstr(c.qrEcc); m["qrSize"] = jnum(c.qrSize);
    m["qrFg"] = jnum(c.qrFg); m["qrBg"] = jnum(c.qrBg);
    m["title"] = jstr(c.title); m["subtitle"] = jstr(c.subtitle);
    m["holdSec"] = jnum(c.holdSec); m["maxLines"] = jnum(c.maxLines); m["widthPct"] = jnum(c.widthPct);
    return o;
}

bool fromJson(const Json& v, ElementConfig& out, const std::string& fallbackType) {
    if (!v.isObj()) return false;
    std::string type = fallbackType;
    getStr(v, "type", type);
    if (!isValidType(type)) return false;
    ElementConfig c = defaultConfig(type);
    getStr(v, "id", c.id); getStr(v, "name", c.name);
    getNum(v, "x", c.x); getNum(v, "y", c.y); getNum(v, "pivotX", c.pivotX); getNum(v, "pivotY", c.pivotY); getInt(v, "z", c.z);
    getNum(v, "size", c.size); getStr(v, "font", c.font); getBool(v, "bold", c.bold);
    getCol(v, "textColor", c.textColor); getCol(v, "bgColor", c.bgColor); getCol(v, "accent", c.accent);
    getNum(v, "bgAlpha", c.bgAlpha); getNum(v, "opacity", c.opacity);
    getStr(v, "animIn", c.animIn); getStr(v, "animOut", c.animOut); getInt(v, "animMs", c.animMs);
    getStr(v, "mode", c.mode); getInt(v, "durationSec", c.durationSec); getStr(v, "format", c.format);
    getBool(v, "blinkAtZero", c.blinkAtZero); getStr(v, "label", c.label);
    getStr(v, "teamA", c.teamA); getStr(v, "teamB", c.teamB); getStr(v, "period", c.period);
    getCol(v, "colorA", c.colorA); getCol(v, "colorB", c.colorB); getInt(v, "scoreA", c.scoreA); getInt(v, "scoreB", c.scoreB);
    getBool(v, "showClock", c.showClock);
    getStr(v, "qrText", c.qrText); getStr(v, "qrCaption", c.qrCaption); getStr(v, "qrEcc", c.qrEcc); getNum(v, "qrSize", c.qrSize);
    getCol(v, "qrFg", c.qrFg); getCol(v, "qrBg", c.qrBg);
    getStr(v, "title", c.title); getStr(v, "subtitle", c.subtitle);
    getNum(v, "holdSec", c.holdSec); getInt(v, "maxLines", c.maxLines); getNum(v, "widthPct", c.widthPct);
    sanitize(c);
    out = std::move(c);
    return true;
}

// ---------------- Element ----------------
Element::Element(ElementConfig cfg) : cfg_(std::move(cfg)) {
    sanitize(cfg_);
    timer_.configure(cfg_.mode == "up" ? Timer::Mode::Up : Timer::Mode::Down, static_cast<std::int64_t>(cfg_.durationSec) * 1000);
}

std::string Element::id() const { std::lock_guard l(mutex_); return cfg_.id; }
int Element::zOrder() const { std::lock_guard l(mutex_); return cfg_.z; }
Placement Element::placement() const {
    std::lock_guard l(mutex_);
    return {cfg_.x, cfg_.y, cfg_.pivotX, cfg_.pivotY};
}
ElementConfig Element::config() const { std::lock_guard l(mutex_); return cfg_; }
std::uint64_t Element::rasterCount() const { std::lock_guard l(mutex_); return rasters_; }

bool Element::visible() const { std::lock_guard l(mutex_); return anim_.target(); }
bool Element::isOn(std::int64_t now) const {
    std::lock_guard l(mutex_);
    return anim_.target() && !(autoHideAt_ >= 0 && now >= autoHideAt_);
}

bool Element::active(const RenderContext& ctx) const {
    std::lock_guard l(mutex_);
    return anim_.active(ctx.nowMs);
}

void Element::applyConfig(const ElementConfig& in, std::int64_t now) {
    std::lock_guard l(mutex_);
    ElementConfig c = in;
    c.id = cfg_.id;      // identity and type never change
    c.type = cfg_.type;
    sanitize(c);
    const bool timerChanged = c.mode != cfg_.mode || c.durationSec != cfg_.durationSec;
    cfg_ = std::move(c);
    if (timerChanged) {
        timer_.reset();
        timer_.configure(cfg_.mode == "up" ? Timer::Mode::Up : Timer::Mode::Down, static_cast<std::int64_t>(cfg_.durationSec) * 1000);
    }
    cardKey_.clear();
    (void)now;
}

void Element::setOn(bool on, std::int64_t now) {
    std::lock_guard l(mutex_);
    if (on) autoHideAt_ = -1;
    if (on && cfg_.type == "chatpin" && cfg_.holdSec > 0 && !chatText_.empty()) autoHideAt_ = now + static_cast<std::int64_t>(cfg_.holdSec * 1000);
    anim_.set(on, now, cfg_.animMs, cfg_.animMs);
}

bool Element::timerCommand(const std::string& cmd, double arg, std::int64_t now) {
    std::lock_guard l(mutex_);
    if (cfg_.type != "timer" && cfg_.type != "scoreboard") return false;
    if (!std::isfinite(arg)) arg = 0;
    if (cmd == "start") timer_.start(now);
    else if (cmd == "pause") timer_.pause(now);
    else if (cmd == "toggle") { if (timer_.running()) timer_.pause(now); else timer_.start(now); }
    else if (cmd == "reset") timer_.reset();
    else if (cmd == "set") {
        cfg_.durationSec = std::clamp(static_cast<int>(arg), 0, 359999);
        timer_.reset();
        timer_.configure(cfg_.mode == "up" ? Timer::Mode::Up : Timer::Mode::Down, static_cast<std::int64_t>(cfg_.durationSec) * 1000);
    } else if (cmd == "add") timer_.adjust(now, static_cast<std::int64_t>(std::clamp(arg, -359999.0, 359999.0) * 1000));
    else return false;
    return true;
}

bool Element::scoreCommand(const std::string& who, int delta, std::int64_t) {
    std::lock_guard l(mutex_);
    if (cfg_.type != "scoreboard") return false;
    if (who == "a") cfg_.scoreA = std::clamp(cfg_.scoreA + delta, 0, 999);
    else if (who == "b") cfg_.scoreB = std::clamp(cfg_.scoreB + delta, 0, 999);
    else if (who == "reset") cfg_.scoreA = cfg_.scoreB = 0;
    else if (who == "swap") { std::swap(cfg_.scoreA, cfg_.scoreB); std::swap(cfg_.teamA, cfg_.teamB); std::swap(cfg_.colorA, cfg_.colorB); }
    else return false;
    return true;
}

void Element::setPeriod(const std::string& text) {
    std::lock_guard l(mutex_);
    if (cfg_.type == "scoreboard") cfg_.period = clip(text, 40);
}

void Element::onCaption(const std::string& textIn, bool final, std::int64_t now) {
    std::lock_guard l(mutex_);
    if (cfg_.type != "captions") return;
    const std::string text = clip(textIn, 600);
    const std::int64_t quiet = static_cast<std::int64_t>(cfg_.holdSec * 1000) + 700;
    if (lastCaptionMs_ >= 0 && now - lastCaptionMs_ > quiet) { committed_.clear(); partial_.clear(); }  // fresh start after silence
    lastCaptionMs_ = now;
    if (final) {
        if (!text.empty()) {
            if (!committed_.empty()) committed_ += ' ';
            committed_ += text;
            if (committed_.size() > 300) {
                std::size_t cut = committed_.size() - 300;
                while (cut < committed_.size() && (static_cast<unsigned char>(committed_[cut]) & 0xC0) == 0x80) ++cut;
                committed_.erase(0, cut);
            }
        }
        partial_.clear();
    } else {
        partial_ = text;
    }
    cardKey_.clear();
}

void Element::onChatPin(const std::string& author, const std::string& text, const std::string& platform, std::int64_t now) {
    std::lock_guard l(mutex_);
    if (cfg_.type != "chatpin") return;
    chatAuthor_ = clip(author, 60);
    chatText_ = clip(text, 400);
    chatPlatform_ = clip(platform, 24);
    cardKey_.clear();
    autoHideAt_ = cfg_.holdSec > 0 ? now + static_cast<std::int64_t>(cfg_.holdSec * 1000) : -1;
    anim_.set(true, now, cfg_.animMs, cfg_.animMs);
}

void Element::liveFields(std::int64_t now, std::string& timerText, bool& running, bool& finished) const {
    std::lock_guard l(mutex_);
    timerText = timer_.text(now, cfg_.format);
    running = timer_.running(now);
    finished = timer_.finished(now);
}

double Element::contentAlpha(std::int64_t now) const {
    if (cfg_.type == "captions") {
        if (lastCaptionMs_ < 0 || (committed_.empty() && partial_.empty())) return 0.0;
        const double silence = static_cast<double>(now - lastCaptionMs_);
        const double hold = cfg_.holdSec * 1000.0;
        if (silence <= hold) return 1.0;
        return std::clamp(1.0 - (silence - hold) / 600.0, 0.0, 1.0);
    }
    if (cfg_.type == "chatpin") return chatText_.empty() ? 0.0 : 1.0;
    if ((cfg_.type == "timer" || (cfg_.type == "scoreboard" && cfg_.showClock)) && cfg_.blinkAtZero && timer_.finished(now))
        return ((now / 500) % 2 == 0) ? 1.0 : 0.25;
    return 1.0;
}

// ---------------- card rasterization ----------------
namespace {

TextStyle makeStyle(const ElementConfig& c, int px, bool bold, std::uint32_t color) {
    TextStyle s;
    s.font = c.font;
    s.px = std::max(6, px);
    s.bold = bold;
    s.color = color;
    return s;
}

// Digits-widest reference ("88:88") so a ticking clock does not make its box jitter.
std::string widest(const std::string& t) {
    std::string r = t;
    for (auto& ch : r) if (ch >= '0' && ch <= '9') ch = '8';
    return r;
}

struct Piece {
    Bitmap bmp;
    bool ok = false;
};

Piece rast(ITextRasterizer& tr, const std::string& text, const TextStyle& st, int maxW = 0) {
    Piece p;
    p.ok = !text.empty() && tr.rasterize(text, st, maxW, p.bmp);
    return p;
}

int measureW(ITextRasterizer& tr, const std::string& text, const TextStyle& st) {
    TextMetrics m;
    return tr.measure(text, st, 0, m) ? m.width : 0;
}

Bitmap newCard(int w, int h) {
    Bitmap b;
    b.resize(std::clamp(w, 1, 8192), std::clamp(h, 1, 4096));
    return b;
}

int alpha255(double a) { return std::clamp(static_cast<int>(std::lround(a * 255.0)), 0, 255); }

}  // namespace

bool Element::buildCard(const RenderContext& ctx, std::int64_t now, std::string& key) {
    const int H = ctx.height, W = ctx.width;
    const int px = std::max(8, roundi(cfg_.size * H));
    const std::string clock = (cfg_.type == "timer" || cfg_.type == "scoreboard") ? timer_.text(now, cfg_.format) : std::string();
    key = cfg_.type + '|' + std::to_string(W) + 'x' + std::to_string(H) + '|' + clock;
    // Content that lives outside cfg_ is part of the key through a hash of the current texts.
    key += '|' + committed_ + '\x1f' + partial_ + '\x1f' + chatAuthor_ + '\x1f' + chatText_ + '\x1f' + chatPlatform_;
    key += '|' + std::to_string(cfg_.scoreA) + ':' + std::to_string(cfg_.scoreB) + cfg_.period + cfg_.teamA + cfg_.teamB;
    if (key == cardKey_ && !card_.empty()) return true;  // unchanged: reuse
    // cfg_ changes clear cardKey_ in applyConfig(), so equality here means nothing visible changed.
    ITextRasterizer& tr = *ctx.text;
    const int pad = std::max(4, px * 3 / 10);
    const int bgA = alpha255(cfg_.bgAlpha);
    Bitmap out;

    if (cfg_.type == "timer") {
        TextStyle ts = makeStyle(cfg_, px, cfg_.bold, cfg_.textColor);
        ts.shadowPx = std::max(1, px / 24);
        const int boxW = std::max(measureW(tr, widest(clock), ts), measureW(tr, clock, ts));
        Piece t = rast(tr, clock, ts);
        TextStyle ls = makeStyle(cfg_, px * 4 / 10, true, cfg_.accent);
        Piece lab = rast(tr, cfg_.label, ls);
        if (!t.ok) return false;
        const int labH = lab.ok ? lab.bmp.h : 0;
        const int w = std::max(boxW, lab.ok ? lab.bmp.w : 0) + 2 * pad;
        const int h = t.bmp.h + labH + 2 * pad;
        out = newCard(w, h);
        fillRect(out, 0, 0, w, h, cfg_.bgColor, bgA);
        fillRect(out, 0, 0, std::max(3, px / 12), h, cfg_.accent, 255);
        int y = pad;
        if (lab.ok) { blendBitmap(out, lab.bmp, (w - lab.bmp.w) / 2, y); y += labH; }
        blendBitmap(out, t.bmp, (w - t.bmp.w) / 2, y);
    } else if (cfg_.type == "scoreboard") {
        TextStyle ns = makeStyle(cfg_, px * 7 / 10, true, cfg_.textColor);
        TextStyle ss = makeStyle(cfg_, px, true, cfg_.textColor);
        const std::string sa = std::to_string(cfg_.scoreA), sb = std::to_string(cfg_.scoreB);
        Piece na = rast(tr, cfg_.teamA, ns), nb = rast(tr, cfg_.teamB, ns);
        Piece a = rast(tr, sa, ss), b = rast(tr, sb, ss);
        if (!a.ok || !b.ok) return false;
        const int rowH = a.bmp.h + pad;
        const int nameW = std::max({na.ok ? na.bmp.w : 0, nb.ok ? nb.bmp.w : 0, px * 2}) + 2 * pad;
        const int scoreW = std::max({a.bmp.w, b.bmp.w, measureW(tr, "88", ss)}) + 2 * pad;
        const int w = 2 * nameW + 2 * scoreW;
        std::string info = cfg_.period;
        if (cfg_.showClock) info += (info.empty() ? "" : "  ") + clock;
        TextStyle is = makeStyle(cfg_, px * 5 / 10, true, cfg_.textColor);
        Piece inf = rast(tr, info, is);
        Piece lab = rast(tr, cfg_.label, makeStyle(cfg_, px * 4 / 10, true, cfg_.accent));
        const int topH = lab.ok ? lab.bmp.h + pad / 2 : 0;
        const int botH = inf.ok ? inf.bmp.h + pad : 0;
        out = newCard(w, topH + rowH + botH);
        int y = 0;
        if (lab.ok) { fillRect(out, 0, 0, w, topH, cfg_.bgColor, std::max(bgA, 150)); blendBitmap(out, lab.bmp, (w - lab.bmp.w) / 2, pad / 4); y = topH; }
        fillRect(out, 0, y, nameW, rowH, cfg_.colorA, 255);
        fillRect(out, nameW, y, scoreW, rowH, cfg_.bgColor, std::max(bgA, 200));
        fillRect(out, nameW + scoreW, y, scoreW, rowH, cfg_.bgColor, std::max(bgA, 200));
        fillRect(out, nameW + 2 * scoreW, y, nameW, rowH, cfg_.colorB, 255);
        const int ty = y + (rowH - a.bmp.h) / 2;
        if (na.ok) blendBitmap(out, na.bmp, (nameW - na.bmp.w) / 2, y + (rowH - na.bmp.h) / 2);
        blendBitmap(out, a.bmp, nameW + (scoreW - a.bmp.w) / 2, ty);
        blendBitmap(out, b.bmp, nameW + scoreW + (scoreW - b.bmp.w) / 2, ty);
        if (nb.ok) blendBitmap(out, nb.bmp, nameW + 2 * scoreW + (nameW - nb.bmp.w) / 2, y + (rowH - nb.bmp.h) / 2);
        if (inf.ok) {
            fillRect(out, 0, y + rowH, w, botH, cfg_.bgColor, std::max(bgA, 150));
            blendBitmap(out, inf.bmp, (w - inf.bmp.w) / 2, y + rowH + pad / 2);
        }
    } else if (cfg_.type == "qr") {
        qr::QrCode code;
        const qr::Ecc ecc = cfg_.qrEcc == "L" ? qr::Ecc::L : cfg_.qrEcc == "Q" ? qr::Ecc::Q : cfg_.qrEcc == "H" ? qr::Ecc::H : qr::Ecc::M;
        if (!qr::encode(cfg_.qrText, ecc, code)) return false;  // empty or too long: nothing to show
        const int quiet = 4;
        const int mods = code.size + 2 * quiet;
        const int mp = std::max(1, roundi(cfg_.qrSize * H) / mods);
        const int qrPx = mods * mp;
        TextStyle cs = makeStyle(cfg_, px, true, cfg_.qrFg);
        Piece cap = rast(tr, cfg_.qrCaption, cs, std::max(qrPx, 80));
        const int w = std::max(qrPx, cap.ok ? cap.bmp.w + 2 * pad : 0);
        const int h = qrPx + (cap.ok ? cap.bmp.h + pad : 0);
        out = newCard(w, h);
        fillRect(out, 0, 0, w, h, cfg_.qrBg, 255);
        const int ox = (w - qrPx) / 2;
        for (int y = 0; y < code.size; ++y)
            for (int x = 0; x < code.size; ++x)
                if (code.get(x, y)) fillRect(out, ox + (x + quiet) * mp, (y + quiet) * mp, mp, mp, cfg_.qrFg, 255);
        if (cap.ok) blendBitmap(out, cap.bmp, (w - cap.bmp.w) / 2, qrPx - quiet * mp / 2);
    } else if (cfg_.type == "lowerthird") {
        TextStyle ts = makeStyle(cfg_, px, true, cfg_.textColor);
        TextStyle ss = makeStyle(cfg_, px * 6 / 10, false, cfg_.textColor);
        Piece t = rast(tr, cfg_.title, ts), s = rast(tr, cfg_.subtitle, ss);
        if (!t.ok && !s.ok) return false;
        const int bar = std::max(4, px / 8);
        const int tw = t.ok ? t.bmp.w : 0, sw = s.ok ? s.bmp.w : 0;
        const int w = std::max(tw, sw) + 2 * pad + bar + pad / 2;
        const int h = (t.ok ? t.bmp.h : 0) + (s.ok ? s.bmp.h : 0) + 2 * pad;
        out = newCard(w, h);
        fillRect(out, 0, 0, w, h, cfg_.bgColor, bgA);
        fillRect(out, 0, 0, bar, h, cfg_.accent, 255);
        int y = pad;
        const int x = bar + pad;
        if (t.ok) { blendBitmap(out, t.bmp, x, y); y += t.bmp.h; }
        if (s.ok) blendBitmap(out, s.bmp, x, y);
    } else if (cfg_.type == "captions") {
        std::string text = committed_;
        if (!partial_.empty()) text += (text.empty() ? "" : " ") + partial_;
        TextStyle st = makeStyle(cfg_, px, cfg_.bold, cfg_.textColor);
        st.align = Align::Center;
        st.maxLines = cfg_.maxLines;
        st.keepTail = true;
        st.shadowPx = std::max(1, px / 20);
        Piece t = rast(tr, text, st, roundi(cfg_.widthPct * W) - 2 * pad);
        if (!t.ok) return false;
        out = newCard(t.bmp.w + 2 * pad, t.bmp.h + pad);
        fillRect(out, 0, 0, out.w, out.h, cfg_.bgColor, bgA);
        blendBitmap(out, t.bmp, pad, pad / 2);
    } else {  // chatpin
        TextStyle as = makeStyle(cfg_, px * 8 / 10, true, cfg_.accent);
        TextStyle ts = makeStyle(cfg_, px, cfg_.bold, cfg_.textColor);
        ts.maxLines = cfg_.maxLines;
        std::string who = chatAuthor_;
        if (!chatPlatform_.empty()) who += "  \xC2\xB7  " + chatPlatform_;
        const int maxW = roundi(cfg_.widthPct * W) - 2 * pad - std::max(4, px / 8);
        Piece a = rast(tr, who, as, maxW), t = rast(tr, chatText_, ts, maxW);
        if (!t.ok) return false;
        const int bar = std::max(4, px / 8);
        const int w = std::max(t.bmp.w, a.ok ? a.bmp.w : 0) + 2 * pad + bar;
        const int h = t.bmp.h + (a.ok ? a.bmp.h : 0) + 2 * pad;
        out = newCard(w, h);
        fillRect(out, 0, 0, w, h, cfg_.bgColor, bgA);
        fillRect(out, 0, 0, bar, h, cfg_.accent, 255);
        int y = pad;
        if (a.ok) { blendBitmap(out, a.bmp, bar + pad, y); y += a.bmp.h; }
        blendBitmap(out, t.bmp, bar + pad, y);
    }
    card_ = std::move(out);
    cardKey_ = key;
    ++rasters_;
    return !card_.empty();
}

void Element::render(video::Frame& canvas, const RenderContext& ctx) {
    std::lock_guard l(mutex_);
    const std::int64_t now = ctx.nowMs;
    if (autoHideAt_ >= 0 && now >= autoHideAt_ && anim_.target()) {
        anim_.set(false, now, cfg_.animMs, cfg_.animMs);
        autoHideAt_ = -1;
    }
    const double p = anim_.progress(now);
    const double ca = contentAlpha(now);
    if (p <= 0.0 || ca <= 0.0 || !ctx.text || !frameValid(canvas)) return;
    RenderContext c = ctx;
    c.width = canvas.width;
    c.height = canvas.height;
    std::string key;
    if (!buildCard(c, now, key) || card_.empty()) return;

    const std::string& anim = anim_.target() ? cfg_.animIn : cfg_.animOut;
    const double e = Animator::ease(p);
    double alpha = cfg_.opacity * ca;
    if (anim == "none") alpha *= 1.0;
    else alpha *= e;
    int x = roundi(cfg_.x * canvas.width - cfg_.pivotX * card_.w);
    int y = roundi(cfg_.y * canvas.height - cfg_.pivotY * card_.h);
    if (anim == "slide-left") x -= roundi((1.0 - e) * (x + card_.w));
    else if (anim == "slide-right") x += roundi((1.0 - e) * (canvas.width - x));
    else if (anim == "slide-up") y += roundi((1.0 - e) * (canvas.height - y));
    else if (anim == "slide-down") y -= roundi((1.0 - e) * (y + card_.h));
    blendBitmap(canvas, card_, x, y, alpha);
}

}  // namespace hb::overlay
