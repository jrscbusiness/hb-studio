#pragma once
#include <chrono>
#include <memory>
#include <mutex>
#include <vector>

#include "core/event_bus.hpp"
#include "video/frame_ops.hpp"
#include "video/source.hpp"

namespace hb::switcher {

// Preview/Program switcher with CUT and DISSOLVE.
//  - setPreview(i): choose the next source.
//  - cut(): swap preview and program immediately.
//  - startDissolve(ms): cross-fade program -> preview, then swap.
//  - setProgram(i): "program bus" direct cut (does not touch preview).
// renderProgram() is called by the Program Output clock; it also completes
// finished transitions. Events: switcher.previewChanged, switcher.programChanged,
// transition.started, transition.finished.
class Switcher {
public:
    using Clock = std::chrono::steady_clock;
    struct State {
        int pvw = 1;
        int pgm = 0;
        bool transitioning = false;
        float progress = 0.f;
    };

    explicit Switcher(std::vector<std::shared_ptr<const video::Source>> sources, core::EventBus* bus = nullptr);

    int sourceCount() const { return static_cast<int>(sources_.size()); }
    const video::Source& source(int i) const { return *sources_[static_cast<std::size_t>(i)]; }

    State state(Clock::time_point now = Clock::now()) const;
    bool setPreview(int i);
    bool setProgram(int i);
    bool cut();
    bool startDissolve(int ms, Clock::time_point now = Clock::now());

    void renderProgram(video::Frame& out, std::uint64_t index, Clock::time_point now = Clock::now());
    void renderPreview(video::Frame& out, std::uint64_t index) const;

private:
    void publish(const char* topic, int a, int b) const;

    std::vector<std::shared_ptr<const video::Source>> sources_;
    core::EventBus* bus_;
    mutable std::mutex mutex_;
    int pvw_ = 1, pgm_ = 0;
    bool transitioning_ = false;
    Clock::time_point start_{};
    std::chrono::milliseconds duration_{500};
    video::Frame scratchA_, scratchB_;
};

}  // namespace hb::switcher
