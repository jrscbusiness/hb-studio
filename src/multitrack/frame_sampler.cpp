#include "multitrack/frame_sampler.hpp"

#include <chrono>

namespace hb::multitrack {

VideoSampler::VideoSampler(int width, int height, int fpsNum, int fpsDen, RenderFn render, SinkFn sink,
                           const SessionClock* clock)
    : w_(width), h_(height), num_(fpsNum > 0 ? fpsNum : 30), den_(fpsDen > 0 ? fpsDen : 1),
      render_(std::move(render)), sink_(std::move(sink)), clock_(clock), pool_(width, height) {}

void VideoSampler::start() {
    if (thread_.joinable() || !render_ || !sink_ || !clock_) return;
    {
        std::lock_guard lk(mu_);
        stop_ = false;
    }
    thread_ = std::thread([this] { loop(); });
}

void VideoSampler::stop() {
    {
        std::lock_guard lk(mu_);
        stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void VideoSampler::loop() {
    using Clock = std::chrono::steady_clock;
    const auto start = Clock::now();
    const auto period = std::chrono::nanoseconds(static_cast<std::int64_t>(1000000000.0 * den_ / num_));
    std::uint64_t n = 0;
    for (;;) {
        const auto due = start + std::chrono::nanoseconds(unitsToNs(n, static_cast<std::uint64_t>(num_), static_cast<std::uint64_t>(den_)));
        {
            std::unique_lock lk(mu_);
            if (cv_.wait_until(lk, due, [&] { return stop_; })) return;
        }
        const auto now = Clock::now();
        if (now > due + 2 * period) {   // fell behind: skip the missed ticks instead of bursting
            const auto missed = static_cast<std::uint64_t>((now - due) / period);
            late_ += missed;
            n += missed;
        }
        try {
            auto f = pool_.acquire();
            const Ns stamp = clock_->nowNs();
            render_(*f, n);
            f->index = n;
            f->ptsNs = stamp;
            sink_(std::move(f), stamp);
        } catch (...) {
            // a faulty source must not kill the sampler: the track just sees a gap and repeats the last frame
        }
        ++ticks_;
        ++n;
    }
}

}  // namespace hb::multitrack
