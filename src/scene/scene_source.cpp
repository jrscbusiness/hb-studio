#include "scene/scene_source.hpp"

#include <algorithm>
#include <cmath>

namespace hb::scene {

SceneSource::SceneSource(std::string name, std::vector<std::shared_ptr<const video::Source>> inputs,
                         std::function<double(int)> aspectOf, ImageLoader loader)
    : name_(std::move(name)), inputs_(std::move(inputs)), aspectOf_(std::move(aspectOf)), loader_(std::move(loader)) {}

void SceneSource::setScene(std::shared_ptr<const Scene> scene) {
    std::lock_guard lock(mutex_);
    scene_ = std::move(scene);
    cache_.clear();
}

std::shared_ptr<const Scene> SceneSource::scene() const {
    std::lock_guard lock(mutex_);
    return scene_;
}

std::uint64_t SceneSource::compileCount() const {
    std::lock_guard lock(mutex_);
    return compiles_;
}

std::vector<double> SceneSource::currentAspects(const Scene& s) const {
    std::vector<double> a(s.layers.size(), 16.0 / 9.0);
    if (!aspectOf_) return a;
    for (std::size_t i = 0; i < s.layers.size(); ++i) {
        const int slot = s.layers[i].slot;
        if (slot < 0) continue;
        double v = 0;
        try { v = aspectOf_(slot); } catch (...) { v = 0; }
        if (v > 0.2 && v < 6.0) a[i] = v;
    }
    return a;
}

std::shared_ptr<const CompiledScene> SceneSource::compiled(const std::shared_ptr<const Scene>& sc, int w, int h,
                                                           const std::vector<double>& aspects) const {
    {
        std::lock_guard lock(mutex_);
        if (scene_ != sc) return nullptr;  // replaced meanwhile: the caller composes nothing this tick
        ++tick_;
        for (auto& e : cache_) {
            if (e.cs->width != w || e.cs->height != h) continue;
            bool same = e.cs->aspects.size() == aspects.size();
            for (std::size_t i = 0; same && i < aspects.size(); ++i)
                same = std::fabs(e.cs->aspects[i] - aspects[i]) < 0.01;
            if (same) { e.used = tick_; return e.cs; }
        }
    }
    auto cs = compileScene(*sc, w, h, aspects, loader_);   // outside the lock: can take a few ms
    std::lock_guard lock(mutex_);
    if (scene_ != sc) return cs;
    ++compiles_;
    // Replace an entry of the same size (aspect changed) or the least recently used one.
    constexpr std::size_t kMaxEntries = 8;
    for (auto& e : cache_)
        if (e.cs->width == w && e.cs->height == h) { e.cs = cs; e.used = ++tick_; return cs; }
    if (cache_.size() >= kMaxEntries) {
        auto it = std::min_element(cache_.begin(), cache_.end(), [](const CacheEntry& a, const CacheEntry& b) { return a.used < b.used; });
        cache_.erase(it);
    }
    cache_.push_back({cs, ++tick_});
    return cs;
}

void SceneSource::prepare(int width, int height) {
    auto sc = scene();
    if (!sc || width <= 0 || height <= 0) return;
    (void)compiled(sc, width, height, currentAspects(*sc));
}

void SceneSource::render(video::Frame& frame, std::uint64_t index) const {
    if (frame.width <= 0 || frame.height <= 0 ||
        frame.data.size() < static_cast<std::size_t>(frame.stride) * static_cast<std::size_t>(frame.height))
        return;
    auto sc = scene();
    std::shared_ptr<const CompiledScene> cs;
    if (sc) cs = compiled(sc, frame.width, frame.height, currentAspects(*sc));
    if (!sc || !cs) {
        // empty slot: calm dark panel (never garbage)
        for (std::size_t i = 0; i + 3 < frame.data.size(); i += 4) {
            frame.data[i] = 36; frame.data[i + 1] = 26; frame.data[i + 2] = 18; frame.data[i + 3] = 255;
        }
        return;
    }
    composeScene(*cs, *sc,
                 [this](const Layer& l, video::Frame& f, std::uint64_t idx) {
                     if (l.slot < 0 || static_cast<std::size_t>(l.slot) >= inputs_.size() || !inputs_[static_cast<std::size_t>(l.slot)])
                         return false;
                     inputs_[static_cast<std::size_t>(l.slot)]->render(f, idx);
                     return true;
                 },
                 frame, index);
}

}  // namespace hb::scene
