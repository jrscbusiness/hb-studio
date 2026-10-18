#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/clock.hpp"
#include "core/event_bus.hpp"

namespace hb::video {

enum class PixelFormat { BGRA8 };

struct Frame {
    int width = 0;
    int height = 0;
    int stride = 0;  // bytes per row
    PixelFormat format = PixelFormat::BGRA8;
    std::vector<std::uint8_t> data;
    std::int64_t ptsNs = 0;
    std::uint64_t index = 0;
};

// Recycles frame buffers so steady-state output performs no allocation.
class FramePool {
public:
    FramePool(int width, int height) : width_(width), height_(height) {}
    std::shared_ptr<Frame> acquire();
    std::size_t idleCount() const;

private:
    struct State {
        std::mutex mutex;
        std::vector<std::unique_ptr<Frame>> idle;
    };
    int width_, height_;
    std::shared_ptr<State> state_ = std::make_shared<State>();
};

struct ProgramFormat {
    int width = 1920;
    int height = 1080;
    core::Rational fps{30, 1};
};

// Renders into the frame for tick `index`. The default renderer draws SMPTE
// style colour bars with a moving marker (used until real sources exist).
using Renderer = std::function<void(Frame& frame, std::uint64_t index)>;
void renderTestPattern(Frame& frame, std::uint64_t index);

// The PROGRAM OUTPUT: a fixed-resolution frame stream produced by the master
// clock, completely independent of any UI window. Consumers (virtual camera,
// recorder, streamer, monitors) receive shared immutable frames.
//
// Isolation: a throwing consumer is counted and ignored. Consumers are
// invoked on the clock thread, so they must hand off quickly (copy the
// shared_ptr into their own queue/mailbox); per-consumer mailboxes are added
// together with the real consumers.
class ProgramOutput {
public:
    using ConsumerId = std::uint64_t;
    using Consumer = std::function<void(const std::shared_ptr<const Frame>&)>;

    explicit ProgramOutput(ProgramFormat format, core::EventBus* bus = nullptr);
    ~ProgramOutput() { stop(); }

    void setRenderer(Renderer renderer);
    void start();
    void stop();
    bool running() const { return clock_.running(); }

    ConsumerId addConsumer(Consumer consumer);
    void removeConsumer(ConsumerId id);

    std::shared_ptr<const Frame> latest() const;
    const ProgramFormat& format() const { return format_; }
    std::uint64_t framesProduced() const { return produced_.load(); }
    std::uint64_t consumerErrors() const { return consumerErrors_.load(); }
    std::uint64_t rendererErrors() const { return rendererErrors_.load(); }
    core::ClockStats clockStats() const { return clock_.stats(); }

private:
    void onTick(std::uint64_t index, core::SteadyClock::time_point deadline);

    ProgramFormat format_;
    core::EventBus* bus_;
    FramePool pool_;
    core::ProgramClock clock_;
    core::SteadyClock::time_point origin_{};

    mutable std::mutex mutex_;
    Renderer renderer_;
    std::shared_ptr<const Frame> latest_;
    std::vector<std::pair<ConsumerId, Consumer>> consumers_;
    ConsumerId nextConsumer_ = 1;
    std::atomic<std::uint64_t> produced_{0}, consumerErrors_{0}, rendererErrors_{0};
};

}  // namespace hb::video
