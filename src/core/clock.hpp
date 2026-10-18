#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>

namespace hb::core {

struct Rational {
    int num = 30;  // frames
    int den = 1;   // per den seconds (e.g. 30000/1001 = 29.97)
    double value() const { return static_cast<double>(num) / den; }
    bool operator==(const Rational&) const = default;
};

using SteadyClock = std::chrono::steady_clock;

// Deadline of tick n, computed from the start instant (never accumulated),
// so there is no cumulative drift.
SteadyClock::time_point tickDeadline(SteadyClock::time_point start, Rational fps,
                                     std::uint64_t n);

struct ClockStats {
    std::uint64_t ticks = 0;          // callbacks delivered
    std::uint64_t skippedTicks = 0;   // ticks dropped because we fell behind
    std::uint64_t callbackErrors = 0; // exceptions swallowed
};

// Master clock of the Program Output. Independent of any window/VSync.
// If the callback overruns, late ticks are skipped (never queued), so the
// output stays in real time instead of drifting.
class ProgramClock {
public:
    using Tick = std::function<void(std::uint64_t index, SteadyClock::time_point deadline)>;

    ~ProgramClock() { stop(); }
    void start(Rational fps, Tick tick);
    void stop();
    bool running() const { return running_.load(); }
    ClockStats stats() const;

private:
    void loop(Rational fps, Tick tick);

    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> ticks_{0}, skipped_{0}, errors_{0};
};

}  // namespace hb::core
