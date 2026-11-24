#include "cloud/throttle.hpp"

#include <algorithm>

namespace hb::cloud {

void TokenBucket::setRate(double bytesPerSec, double burstBytes) {
    std::lock_guard lock(mu_);
    rate_ = bytesPerSec > 0 ? bytesPerSec : 0;
    burst_ = burstBytes > 0 ? burstBytes : std::max(rate_ / 4.0, 64.0 * 1024.0);
    tokens_ = std::min(tokens_, burst_);
    if (!primed_) {
        tokens_ = burst_;  // start full: the first chunk goes out at once
        last_ = now_();
        primed_ = true;
    }
}

double TokenBucket::rate() const {
    std::lock_guard lock(mu_);
    return rate_;
}

double TokenBucket::tryTake(double n) {
    std::lock_guard lock(mu_);
    if (rate_ <= 0) return 0;
    const double t = now_();
    if (!primed_) {
        tokens_ = burst_;
        primed_ = true;
    } else if (t > last_) {
        tokens_ = std::min(burst_, tokens_ + (t - last_) * rate_);
    }
    last_ = t;
    n = std::min(n, burst_);
    if (tokens_ >= n) {
        tokens_ -= n;
        return 0;
    }
    return (n - tokens_) / rate_;
}

}  // namespace hb::cloud
