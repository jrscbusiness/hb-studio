#pragma once
// Pure, clock-injected state machines used by the overlay elements (all times are milliseconds on an
// arbitrary monotonic clock supplied by the caller, so tests are deterministic).
#include <algorithm>
#include <cstdint>
#include <string>

namespace hb::overlay {

// Count-down / count-up timer with start / pause / reset. Count-down stops itself at zero.
class Timer {
public:
    enum class Mode { Down, Up };

    void configure(Mode mode, std::int64_t durationMs) {
        mode_ = mode;
        durationMs_ = std::max<std::int64_t>(0, durationMs);
    }
    void start(std::int64_t now) {
        if (running_ || finished(now)) return;
        running_ = true;
        startedAt_ = now;
    }
    void pause(std::int64_t now) {
        if (!running_) return;
        accumulated_ += now - startedAt_;
        running_ = false;
    }
    void reset() { running_ = false; accumulated_ = 0; }
    // Shifts the remaining time (count-down) or the elapsed time (count-up) by deltaMs.
    void adjust(std::int64_t now, std::int64_t deltaMs) {
        const std::int64_t e = elapsedMs(now);
        std::int64_t target = mode_ == Mode::Down ? e - deltaMs : e + deltaMs;
        target = std::max<std::int64_t>(0, target);
        if (mode_ == Mode::Down) target = std::min(target, durationMs_);
        accumulated_ = target;
        startedAt_ = now;
    }
    bool running(std::int64_t now) const { return running_ && !finished(now); }
    bool running() const { return running_; }
    Mode mode() const { return mode_; }
    std::int64_t durationMs() const { return durationMs_; }

    std::int64_t elapsedMs(std::int64_t now) const {
        std::int64_t e = accumulated_ + (running_ ? std::max<std::int64_t>(0, now - startedAt_) : 0);
        if (mode_ == Mode::Down) e = std::min(e, durationMs_);
        return e;
    }
    // Count-down reached zero (only meaningful for a non-zero duration).
    bool finished(std::int64_t now) const { return mode_ == Mode::Down && durationMs_ > 0 && elapsedMs(now) >= durationMs_; }

    // Whole seconds shown on screen: count-down rounds UP (so 00:00 only appears at the very end),
    // count-up rounds down.
    std::int64_t displaySeconds(std::int64_t now) const {
        if (mode_ == Mode::Down) {
            const std::int64_t rem = durationMs_ - elapsedMs(now);
            return (rem + 999) / 1000;
        }
        return elapsedMs(now) / 1000;
    }

    // fmt: "auto" (hh:mm:ss only when needed), "mm:ss" (minutes may exceed 59), "hh:mm:ss".
    static std::string format(std::int64_t seconds, const std::string& fmt) {
        seconds = std::max<std::int64_t>(0, seconds);
        const long long h = seconds / 3600, m = (seconds % 3600) / 60, s = seconds % 60;
        auto two = [](long long v) { return (v < 10 ? "0" : "") + std::to_string(v); };
        if (fmt == "hh:mm:ss" || (fmt != "mm:ss" && h > 0)) return two(h) + ":" + two(m) + ":" + two(s);
        return two(seconds / 60) + ":" + two(s);
    }
    std::string text(std::int64_t now, const std::string& fmt) const { return format(displaySeconds(now), fmt); }

private:
    Mode mode_ = Mode::Down;
    std::int64_t durationMs_ = 0, accumulated_ = 0, startedAt_ = 0;
    bool running_ = false;
};

// On-air animation: progress goes 0 -> 1 when shown (animInMs) and 1 -> 0 when hidden (animOutMs);
// reversing mid-way continues from the current progress (no jump).
class Animator {
public:
    void set(bool on, std::int64_t now, int inMs, int outMs) {
        if (on == target_ && started_) return;
        const double p = progress(now);
        from_ = p;
        t0_ = now;
        target_ = on;
        started_ = true;
        durMs_ = std::max(0, on ? inMs : outMs);
    }
    bool target() const { return target_; }
    double progress(std::int64_t now) const {
        if (!started_) return 0.0;
        const double goal = target_ ? 1.0 : 0.0;
        if (durMs_ <= 0) return goal;
        const double k = std::clamp(static_cast<double>(now - t0_) / durMs_, 0.0, 1.0);
        return from_ + (goal - from_) * k;
    }
    bool animating(std::int64_t now) const { return started_ && durMs_ > 0 && now - t0_ < durMs_; }
    // Visible at all (shown or still fading out).
    bool active(std::int64_t now) const { return progress(now) > 0.0 || target_; }
    static double ease(double p) { return p * p * (3.0 - 2.0 * p); }  // smoothstep

private:
    bool target_ = false, started_ = false;
    double from_ = 0.0;
    std::int64_t t0_ = 0;
    int durMs_ = 0;
};

}  // namespace hb::overlay
