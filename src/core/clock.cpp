#include "core/clock.hpp"

namespace hb::core {

SteadyClock::time_point tickDeadline(SteadyClock::time_point start, Rational fps,
                                     std::uint64_t n) {
    constexpr std::int64_t kNs = 1'000'000'000;
    const auto num = static_cast<std::uint64_t>(fps.num);
    const auto den = static_cast<std::uint64_t>(fps.den);
    // n * den * 1e9 / num, split to avoid 64-bit overflow for long sessions.
    const std::uint64_t whole = n / num;
    const std::uint64_t rem = n % num;
    const std::int64_t ns = static_cast<std::int64_t>(whole * den) * kNs +
                            static_cast<std::int64_t>(rem * den * kNs / num);
    return start + std::chrono::nanoseconds(ns);
}

void ProgramClock::start(Rational fps, Tick tick) {
    stop();
    if (fps.num <= 0 || fps.den <= 0 || !tick) return;
    ticks_ = skipped_ = errors_ = 0;
    running_ = true;
    thread_ = std::thread([this, fps, tick = std::move(tick)] { loop(fps, tick); });
}

void ProgramClock::stop() {
    {
        std::lock_guard lock(mutex_);
        running_ = false;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

ClockStats ProgramClock::stats() const {
    return {ticks_.load(), skipped_.load(), errors_.load()};
}

void ProgramClock::loop(Rational fps, Tick tick) {
    const auto start = SteadyClock::now();
    std::uint64_t n = 0;
    while (running_) {
        const auto deadline = tickDeadline(start, fps, n);
        {
            std::unique_lock lock(mutex_);
            if (cv_.wait_until(lock, deadline, [this] { return !running_.load(); })) break;
        }
        try {
            tick(n, deadline);
        } catch (...) {
            errors_.fetch_add(1);
        }
        ticks_.fetch_add(1);

        // Fell behind? Jump to the tick that is current instead of bursting.
        const auto now = SteadyClock::now();
        std::uint64_t next = n + 1;
        while (tickDeadline(start, fps, next) <= now) ++next;
        skipped_.fetch_add(next - (n + 1));
        n = next;
    }
}

}  // namespace hb::core
