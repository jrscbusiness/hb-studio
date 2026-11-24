#pragma once
// Compositing contract of the overlay system (owned by the overlay module; the scenes/templates
// compositor may reuse it):
//
//   * A Layer draws itself over a Program-sized, opaque BGRA Frame (see blend.hpp for the alpha rules).
//   * LayerStack::renderOver() is called by the Program renderer AFTER the switcher has rendered the
//     Program frame, on the Program clock thread. It must be cheap: layers draw cached bitmaps and touch
//     only their own dirty rectangle; with no active layer it returns immediately without touching memory.
//   * Layers are drawn in ascending z-order (ties: insertion order). All LayerStack methods are thread-safe.
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "overlay/text.hpp"

namespace hb::overlay {

struct RenderContext {
    int width = 0, height = 0;       // canvas size in pixels
    std::int64_t nowMs = 0;          // monotonic clock, milliseconds
    ITextRasterizer* text = nullptr;  // may be null: text elements then draw nothing
    bool preview = false;            // true when drawing on the Program-sized preview
};

// Normalized placement: (x,y) is the anchor point on the canvas in [0,1]; (pivotX,pivotY) is the point
// of the layer's own box placed on it (0,0 = top-left, 0.5,0.5 = centre, 1,1 = bottom-right).
struct Placement {
    double x = 0.5, y = 0.5;
    double pivotX = 0.5, pivotY = 0.5;
};

class Layer {
public:
    virtual ~Layer() = default;
    virtual std::string id() const = 0;
    virtual bool visible() const = 0;                         // requested on-air state
    virtual int zOrder() const = 0;
    virtual Placement placement() const = 0;
    virtual bool active(const RenderContext& ctx) const = 0;  // visible or still fading out
    virtual void render(video::Frame& canvas, const RenderContext& ctx) = 0;
};

class LayerStack {
public:
    // Adds the layer, replacing any layer with the same id.
    void add(std::shared_ptr<Layer> layer);
    bool remove(const std::string& id);
    std::shared_ptr<Layer> get(const std::string& id) const;
    std::vector<std::string> ids() const;  // in draw order
    // Re-reads every layer's zOrder() (call after a layer's z changed).
    void resort();
    bool anyActive(const RenderContext& ctx) const;
    // Draws all active layers over the canvas. Never throws.
    void renderOver(video::Frame& canvas, const RenderContext& ctx) const;
    std::size_t size() const;

private:
    struct Entry {
        std::shared_ptr<Layer> layer;
        int z = 0;
        std::uint64_t seq = 0;
    };
    using Snapshot = std::vector<Entry>;
    void rebuild();  // callers hold mutex_
    mutable std::mutex mutex_;
    std::vector<Entry> entries_;
    std::shared_ptr<const Snapshot> snapshot_ = std::make_shared<Snapshot>();
    std::uint64_t nextSeq_ = 0;
};

}  // namespace hb::overlay
