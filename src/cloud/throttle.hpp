#pragma once
// Bandwidth throttle: a token bucket with an injectable clock (deterministic tests).
#include <functional>
#include <mutex>

namespace hb::cloud {

class TokenBucket {
public:
    using NowFn = std::function<double()>;  // monotonic seconds
    explicit TokenBucket(NowFn now) : now_(std::move(now)) {}

    // bytesPerSec <= 0 means unlimited. burstBytes <= 0 picks max(rate/4, 64 KiB): a quarter of a
    // second of traffic, small enough that the cap is honoured tightly.
    void setRate(double bytesPerSec, double burstBytes = 0);
    double rate() const;

    // Tries to take `n` tokens (n is clamped to the burst size so a request larger than the bucket
    // can still be granted). Returns 0 when granted (tokens consumed); otherwise the number of
    // seconds to wait before trying again (nothing consumed).
    double tryTake(double n);

private:
    mutable std::mutex mu_;
    NowFn now_;
    double rate_ = 0, burst_ = 0, tokens_ = 0, last_ = 0;
    bool primed_ = false;
};

// Mbps (megabits per second, decimal) -> bytes per second.
inline double mbpsToBytesPerSec(double mbps) { return mbps * 1000.0 * 1000.0 / 8.0; }

}  // namespace hb::cloud
