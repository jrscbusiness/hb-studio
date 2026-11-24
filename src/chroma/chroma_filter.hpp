#pragma once
// The per-slot FrameFilter: classic chroma key or AI background removal + replacement.
// apply() runs on whichever thread renders the slot (the 30 fps program clock for PGM), so it does
// only bounded per-pixel work: key/upscale + composite. All inference is on AiMaskWorker's thread.
#include <memory>
#include <mutex>
#include <vector>

#include "chroma/ai_worker.hpp"
#include "chroma/compositor.hpp"
#include "chroma/types.hpp"
#include "video/source.hpp"

namespace hb::chroma {

class ChromaFilter final : public video::FrameFilter {
public:
    ChromaFilter() = default;
    ~ChromaFilter() override { shutdown(); }

    void setSettings(const Settings& s);
    Settings settings() const;
    // The shared model runner; null means "AI unavailable" (model missing / runtime not in this build).
    void setSegmenter(std::shared_ptr<ISegmenter> seg);
    // Decoded replacement image at its native size (opaque BGRA); null clears it.
    void setImage(std::shared_ptr<const video::Frame> img);

    void apply(video::Frame& frame) override;

    struct Stats {
        double applyMsAvg = 0;      // EMA of apply() on the rendering thread (the number that matters for the clock)
        double inferMsAvg = 0;      // EMA of one inference on the worker thread
        std::uint64_t inferences = 0, dropped = 0;
        bool aiActive = false;      // AI mode selected AND a worker is running
        bool aiFailed = false;
        bool aiWaiting = false;     // AI selected but no segmenter (model not ready / no runtime)
        std::string aiMessage;
    };
    Stats stats() const;

    void shutdown();   // joins the worker (call from a non-realtime thread, e.g. at exit)

private:
    std::shared_ptr<const video::Frame> backgroundFor(const video::Frame& frame, const Settings& s,
                                                      std::shared_ptr<const video::Frame> image);
    void rebuildWorkerLocked();

    mutable std::mutex m_;
    Settings s_;
    std::shared_ptr<ISegmenter> seg_;
    std::shared_ptr<AiMaskWorker> worker_;
    std::shared_ptr<const video::Frame> image_;

    struct CacheEntry {
        int w = 0, h = 0, age = 0;
        std::shared_ptr<const video::Frame> frame;
    };
    std::vector<CacheEntry> blurCache_, imageCache_;
    int maxWidthSeen_ = 0;
    double applyMsAvg_ = 0;
};

}  // namespace hb::chroma
