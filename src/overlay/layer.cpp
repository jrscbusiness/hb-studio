#include "overlay/layer.hpp"

#include <algorithm>

namespace hb::overlay {

void LayerStack::rebuild() {
    std::stable_sort(entries_.begin(), entries_.end(), [](const Entry& a, const Entry& b) {
        return a.z != b.z ? a.z < b.z : a.seq < b.seq;
    });
    snapshot_ = std::make_shared<const Snapshot>(entries_);
}

void LayerStack::add(std::shared_ptr<Layer> layer) {
    if (!layer) return;
    std::lock_guard lock(mutex_);
    const std::string id = layer->id();
    std::uint64_t seq = nextSeq_++;
    for (auto it = entries_.begin(); it != entries_.end(); ++it)
        if (it->layer->id() == id) { seq = it->seq; entries_.erase(it); break; }
    entries_.push_back({layer, layer->zOrder(), seq});
    rebuild();
}

bool LayerStack::remove(const std::string& id) {
    std::lock_guard lock(mutex_);
    for (auto it = entries_.begin(); it != entries_.end(); ++it)
        if (it->layer->id() == id) { entries_.erase(it); rebuild(); return true; }
    return false;
}

std::shared_ptr<Layer> LayerStack::get(const std::string& id) const {
    std::lock_guard lock(mutex_);
    for (const auto& e : entries_) if (e.layer->id() == id) return e.layer;
    return nullptr;
}

std::vector<std::string> LayerStack::ids() const {
    std::lock_guard lock(mutex_);
    std::vector<std::string> out;
    for (const auto& e : entries_) out.push_back(e.layer->id());
    return out;
}

void LayerStack::resort() {
    std::lock_guard lock(mutex_);
    for (auto& e : entries_) e.z = e.layer->zOrder();
    rebuild();
}

std::size_t LayerStack::size() const {
    std::lock_guard lock(mutex_);
    return entries_.size();
}

bool LayerStack::anyActive(const RenderContext& ctx) const {
    std::shared_ptr<const Snapshot> snap;
    { std::lock_guard lock(mutex_); snap = snapshot_; }
    for (const auto& e : *snap) if (e.layer->active(ctx)) return true;
    return false;
}

void LayerStack::renderOver(video::Frame& canvas, const RenderContext& ctx) const {
    std::shared_ptr<const Snapshot> snap;
    { std::lock_guard lock(mutex_); snap = snapshot_; }
    for (const auto& e : *snap) {
        try {
            if (e.layer->active(ctx)) e.layer->render(canvas, ctx);
        } catch (...) {
            // a broken layer must never take the Program clock down
        }
    }
}

}  // namespace hb::overlay
