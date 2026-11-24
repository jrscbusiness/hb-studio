#pragma once
// Samples a camera at a constant rate from the session clock: tick n is due at start + n/fps (never
// accumulated, so no drift), renders the CLEAN camera (before overlays/transitions) into a pooled frame and
// hands it to a sink together with the session-clock stamp. One thread per camera.
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

#include "multitrack/session_clock.hpp"
#include "multitrack/track_timeline.hpp"
#include "video/program_output.hpp"

namespace hb::multitrack {

class VideoSampler {
public:
    using RenderFn = std::function<void(video::Frame&, std::uint64_t index)>;
    using SinkFn = std::function<void(std::shared_ptr<const video::Frame>, Ns captureNs)>;

    VideoSampler(int width, int height, int fpsNum, int fpsDen, RenderFn render, SinkFn sink, const SessionClock* clock);
    ~VideoSampler() { stop(); }
    VideoSampler(const VideoSampler&) = delete;
    VideoSampler& operator=(const VideoSampler&) = delete;

    void start();
    void stop();
    std::uint64_t ticks() const { return ticks_.load(); }
    std::uint64_t lateTicks() const { return late_.load(); }   // ticks skipped because the thread fell behind

private:
    void loop();
    int w_, h_, num_, den_;
    RenderFn render_;
    SinkFn sink_;
    const SessionClock* clock_;
    video::FramePool pool_;
    std::thread thread_;
    std::mutex mu_;
    std::condition_variable cv_;
    bool stop_ = false;
    std::atomic<std::uint64_t> ticks_{0}, late_{0};
};

}  // namespace hb::multitrack
