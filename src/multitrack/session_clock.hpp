#pragma once
// The ONE clock every ISO track of a recording session is stamped from. Monotonic (steady_clock): immune to
// wall-clock changes during a long event. The wall-clock time of the start is recorded once, only so the
// manifest can say "this session started at 14:05:33 on 2026-11-24"; it is never used for sync.
#include <chrono>
#include <cstdint>
#include <functional>

namespace hb::multitrack {

class SessionClock {
public:
    using NowFn = std::function<std::int64_t()>;       // monotonic nanoseconds
    using WallFn = std::function<std::int64_t()>;      // unix milliseconds

    explicit SessionClock(NowFn now = {}, WallFn wall = {}) : now_(std::move(now)), wall_(std::move(wall)) {
        if (!now_)
            now_ = [] {
                return static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                     std::chrono::steady_clock::now().time_since_epoch())
                                                     .count());
            };
        if (!wall_)
            wall_ = [] {
                return static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                     std::chrono::system_clock::now().time_since_epoch())
                                                     .count());
            };
    }

    // Defines session time zero. Call once, before any track is fed.
    void start() {
        t0_ = now_();
        wall0Ms_ = wall_();
        started_ = true;
    }
    bool started() const { return started_; }
    // Absolute monotonic reading (same clock as Recorder's cfg.nowNs by default).
    std::int64_t absoluteNs() const { return now_(); }
    std::int64_t t0Ns() const { return t0_; }
    std::int64_t wallStartMs() const { return wall0Ms_; }
    // Nanoseconds since start(): THE capture stamp.
    std::int64_t nowNs() const { return now_() - t0_; }
    // Converts an absolute monotonic reading to session time.
    std::int64_t toSession(std::int64_t absoluteNs) const { return absoluteNs - t0_; }

private:
    NowFn now_;
    WallFn wall_;
    std::int64_t t0_ = 0, wall0Ms_ = 0;
    bool started_ = false;
};

}  // namespace hb::multitrack
