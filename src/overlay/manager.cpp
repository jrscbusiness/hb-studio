#include "overlay/manager.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "qr/qr.hpp"

namespace hb::overlay {

namespace {
using Json = core::json::Value;

std::int64_t steadyMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Splits "a|b|c" into at most maxParts pieces; the last keeps the remaining '|'.
std::vector<std::string> splitCmd(const std::string& s, std::size_t maxParts) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (true) {
        const std::size_t bar = s.find('|', start);
        if (bar == std::string::npos || out.size() + 1 >= maxParts) { out.push_back(s.substr(start)); break; }
        out.push_back(s.substr(start, bar - start));
        start = bar + 1;
    }
    return out;
}

double toNum(const std::string& s, double def = 0) {
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    return (end == s.c_str() || !std::isfinite(v)) ? def : v;
}
}  // namespace

void OverlayManager::Shared::draw(video::Frame& f, bool preview) const {
    std::shared_ptr<ITextRasterizer> t;
    { std::lock_guard lock(mutex); t = text; }
    RenderContext ctx;
    ctx.width = f.width;
    ctx.height = f.height;
    ctx.nowMs = now();
    ctx.text = t.get();
    ctx.preview = preview;
    stack->renderOver(f, ctx);
}

OverlayManager::OverlayManager(core::EventBus* bus, NowFn now, std::shared_ptr<ITextRasterizer> text)
    : bus_(bus), shared_(std::make_shared<Shared>()) {
    shared_->now = now ? std::move(now) : NowFn(steadyMs);
    shared_->text = text ? std::move(text) : makeBuiltinTextRasterizer();
    if (bus_) {
        capSub_ = bus_->subscribe("caption.text", [this](const core::Event& e) { onCaption(e); });
        chatSub_ = bus_->subscribe("overlay.chatPin", [this](const core::Event& e) { onChatPin(e); });
    }
}

OverlayManager::~OverlayManager() {
    if (bus_) {
        bus_->unsubscribe(capSub_);
        bus_->unsubscribe(chatSub_);
    }
}

void OverlayManager::setTextRasterizer(std::shared_ptr<ITextRasterizer> text) {
    if (!text) return;
    std::lock_guard lock(shared_->mutex);
    shared_->text = std::move(text);
}

std::function<void(video::Frame&, std::uint64_t)> OverlayManager::programHook() const {
    auto sh = shared_;
    return [sh](video::Frame& f, std::uint64_t) { sh->draw(f, false); };
}

void OverlayManager::renderPreview(video::Frame& frame) const {
    if (shared_->previewOverlay.load()) shared_->draw(frame, true);
}

void OverlayManager::setPreviewOverlay(bool on) {
    if (shared_->previewOverlay.exchange(on) != on) dirty_ = true;
}

std::vector<std::shared_ptr<Element>> OverlayManager::elementsSnapshot() const {
    std::lock_guard lock(mutex_);
    std::vector<std::shared_ptr<Element>> v;
    for (const auto& kv : elements_) v.push_back(kv.second);
    return v;
}

std::vector<std::string> OverlayManager::ids() const { return shared_->stack->ids(); }

std::shared_ptr<Element> OverlayManager::element(const std::string& id) const {
    std::lock_guard lock(mutex_);
    auto it = elements_.find(id);
    return it == elements_.end() ? nullptr : it->second;
}

void OverlayManager::checkQr(const ElementConfig& c) {
    if (c.type != "qr") return;
    qr::QrCode code;
    const bool ok = qr::encode(c.qrText, c.qrEcc == "L" ? qr::Ecc::L : c.qrEcc == "Q" ? qr::Ecc::Q : c.qrEcc == "H" ? qr::Ecc::H : qr::Ecc::M, code);
    std::lock_guard lock(mutex_);
    qrOk_[c.id] = ok;
}

std::string OverlayManager::upsert(ElementConfig cfg) {
    sanitize(cfg);
    const std::int64_t t = now();
    std::shared_ptr<Element> el;
    {
        std::lock_guard lock(mutex_);
        if (!cfg.id.empty()) {
            auto it = elements_.find(cfg.id);
            if (it != elements_.end()) el = it->second;
        }
        if (!el) {
            if (elements_.size() >= kMaxElements) return {};
            if (cfg.id.empty() || elements_.count(cfg.id)) {
                do { cfg.id = "ov" + std::to_string(nextId_++); } while (elements_.count(cfg.id));
            }
            // keep numeric ids ahead of loaded ones
            if (cfg.id.rfind("ov", 0) == 0) {
                const int n = std::atoi(cfg.id.c_str() + 2);
                if (n >= nextId_) nextId_ = n + 1;
            }
            el = std::make_shared<Element>(cfg);
            elements_[cfg.id] = el;
        }
    }
    if (el->id() == cfg.id && el->config().type == cfg.type) {
        el->applyConfig(cfg, t);
    }
    shared_->stack->add(el);  // re-adding keeps the insertion order and re-reads z
    checkQr(cfg);
    dirty_ = true;
    return cfg.id;
}

bool OverlayManager::remove(const std::string& id) {
    {
        std::lock_guard lock(mutex_);
        if (!elements_.erase(id)) return false;
        qrOk_.erase(id);
    }
    shared_->stack->remove(id);
    dirty_ = true;
    return true;
}

bool OverlayManager::setOn(const std::string& id, bool on) {
    auto el = element(id);
    if (!el) return false;
    el->setOn(on, now());
    return true;  // on-air state is intentionally not persisted
}

bool OverlayManager::timerCommand(const std::string& id, const std::string& cmd, double arg) {
    auto el = element(id);
    if (!el || !el->timerCommand(cmd, arg, now())) return false;
    if (cmd == "set") dirty_ = true;
    return true;
}

bool OverlayManager::scoreCommand(const std::string& id, const std::string& who, int delta) {
    auto el = element(id);
    if (!el || !el->scoreCommand(who, delta, now())) return false;
    dirty_ = true;
    return true;
}

bool OverlayManager::move(const std::string& id, int direction) {
    if (!element(id)) return false;
    // Re-number z as 10,20,30... in the current draw order, then swap with the neighbour.
    auto order = shared_->stack->ids();
    auto it = std::find(order.begin(), order.end(), id);
    if (it == order.end()) return false;
    const auto i = static_cast<std::ptrdiff_t>(it - order.begin());
    const auto j = i + (direction > 0 ? 1 : -1);
    if (j < 0 || j >= static_cast<std::ptrdiff_t>(order.size())) return true;
    std::swap(order[static_cast<std::size_t>(i)], order[static_cast<std::size_t>(j)]);
    int z = 10;
    for (const auto& oid : order) {
        if (auto el = element(oid)) {
            ElementConfig c = el->config();
            c.z = z;
            el->applyConfig(c, now());
        }
        z += 10;
    }
    shared_->stack->resort();
    dirty_ = true;
    return true;
}

void OverlayManager::onCaption(const core::Event& e) {
    auto text = e.fields.find("text");
    if (text == e.fields.end()) return;
    auto fin = e.fields.find("final");
    const bool final = fin != e.fields.end() && fin->second == "1";
    const std::int64_t t = now();
    for (auto& el : elementsSnapshot()) el->onCaption(text->second, final, t);
}

void OverlayManager::onChatPin(const core::Event& e) {
    auto get = [&](const char* k) { auto it = e.fields.find(k); return it == e.fields.end() ? std::string() : it->second; };
    const std::string text = get("text");
    if (text.empty()) return;
    const std::int64_t t = now();
    for (auto& el : elementsSnapshot()) el->onChatPin(get("author"), text, get("platform"), t);
}

bool OverlayManager::handleCommand(const std::string& raw) {
    if (raw.size() < 3 || raw[0] != 'o' || raw[1] != 'v') return false;
    try {
        const std::string c0 = raw.substr(0, raw.find('|'));
        const std::size_t maxParts = c0 == "ovsave" ? 2 : (c0 == "ovperiod" ? 3 : 5);
        const auto a = splitCmd(raw, maxParts);
        const auto arg = [&](std::size_t k) { return k < a.size() ? a[k] : std::string(); };
        if (c0 == "ovsave") {
            Json v;
            ElementConfig cfg;
            if (a.size() >= 2 && core::json::parse(a[1], v, core::json::Limits{64 * 1024, 8, 64}) && fromJson(v, cfg)) upsert(cfg);
        } else if (c0 == "ovnew") {
            if (isValidType(arg(1))) upsert(defaultConfig(arg(1)));
        } else if (c0 == "ovdel") remove(arg(1));
        else if (c0 == "ovshow") setOn(arg(1), arg(2) == "1");
        else if (c0 == "ovtimer") timerCommand(arg(1), arg(2), toNum(arg(3)));
        else if (c0 == "ovscore") {
            const std::string who = arg(2);
            const std::string d = arg(3);
            scoreCommand(arg(1), who, d.empty() ? 1 : static_cast<int>(std::clamp(toNum(d, 1), -999.0, 999.0)));
        } else if (c0 == "ovperiod") {
            if (auto el = element(arg(1))) { el->setPeriod(arg(2)); dirty_ = true; }
        } else if (c0 == "ovmove") move(arg(1), arg(2) == "up" ? 1 : -1);
        else if (c0 == "ovpreview") setPreviewOverlay(arg(1) == "1");
    } catch (...) {
    }
    return true;
}

std::string OverlayManager::stateJson() const {
    const std::int64_t t = now();
    Json root;
    root.t = Json::T::Obj;
    root.o["t"].t = Json::T::Str;
    root.o["t"].s = "overlay_state";
    root.o["preview"].t = Json::T::Bool;
    root.o["preview"].b = previewOverlay();
    Json arr;
    arr.t = Json::T::Arr;
    for (const auto& id : shared_->stack->ids()) {
        auto el = element(id);
        if (!el) continue;
        Json o = toJson(el->config());
        std::string txt;
        bool running = false, finished = false;
        el->liveFields(t, txt, running, finished);
        auto setB = [&](const char* k, bool b) { Json v; v.t = Json::T::Bool; v.b = b; o.o[k] = v; };
        Json s; s.t = Json::T::Str; s.s = txt; o.o["clock"] = s;
        setB("on", el->isOn(t)); setB("running", running); setB("finished", finished);
        {
            std::lock_guard lock(mutex_);
            auto it = qrOk_.find(id);
            setB("qrOk", it == qrOk_.end() ? true : it->second);
        }
        arr.a.push_back(std::move(o));
    }
    root.o["elements"] = std::move(arr);
    return core::json::dump(root);
}

std::string OverlayManager::configJson() const {
    Json root;
    root.t = Json::T::Obj;
    root.o["version"].t = Json::T::Num;
    root.o["version"].n = 1;
    root.o["previewOverlay"].t = Json::T::Bool;
    root.o["previewOverlay"].b = previewOverlay();
    Json arr;
    arr.t = Json::T::Arr;
    for (const auto& id : shared_->stack->ids())
        if (auto el = element(id)) arr.a.push_back(toJson(el->config()));
    root.o["elements"] = std::move(arr);
    return core::json::dump(root, true);
}

bool OverlayManager::loadJsonText(const std::string& text) {
    Json root;
    if (!core::json::parse(text, root) || !root.isObj()) return false;
    if (auto* p = root.get("previewOverlay"); p && p->isBool()) shared_->previewOverlay = p->b;
    const Json* arr = root.get("elements");
    if (!arr || !arr->isArr()) return true;
    for (const auto& v : arr->a) {
        ElementConfig cfg;
        if (!fromJson(v, cfg) || cfg.id.empty()) continue;  // skip damaged entries, keep the rest
        upsert(cfg);
    }
    dirty_ = false;
    return true;
}

bool OverlayManager::load(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    return loadJsonText(ss.str());
}

bool OverlayManager::save(const std::string& path) const {
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << configJson();
        if (!f) return false;
    }
    std::remove(path.c_str());  // std::rename does not overwrite on Windows
    return std::rename(tmp.c_str(), path.c_str()) == 0;
}

}  // namespace hb::overlay
