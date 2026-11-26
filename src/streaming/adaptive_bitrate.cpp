#include "streaming/adaptive_bitrate.hpp"

#include <algorithm>
#include <cmath>

namespace hb::streaming {

AdaptiveBitrate::AdaptiveBitrate(const AbrConfig& cfg) : cfg_(cfg) {
    cfg_.targetKbps = std::max(1, cfg_.targetKbps);
    cfg_.minFraction = std::clamp(cfg_.minFraction, 0.05, 1.0);
    cfg_.stepDown = std::clamp(cfg_.stepDown, 0.3, 0.99);
    cfg_.stressTicksToDown = std::max(1, cfg_.stressTicksToDown);
    min_ = std::max(1, static_cast<int>(std::lround(cfg_.targetKbps * cfg_.minFraction)));
    cur_ = cfg_.targetKbps;
}

void AdaptiveBitrate::reset() {
    cur_ = cfg_.targetKbps;
    stressTicks_ = 0;
    changedOnce_ = false;
    lastChangeMs_ = 0;
    healthySinceMs_ = -1;
    prevDepth_ = 0;
    prevDropped_ = 0;
    haveDropBaseline_ = false;
}

int AdaptiveBitrate::update(std::int64_t nowMs, const AbrSample& s) {
    const double cap = std::max(1, s.queueCapacity);
    const double fill = s.queueDepth / cap;
    const bool newDrops = haveDropBaseline_ && s.droppedTotal > prevDropped_;
    const bool growing = s.queueDepth > prevDepth_;
    haveDropBaseline_ = true;
    prevDropped_ = s.droppedTotal;
    prevDepth_ = s.queueDepth;

    const bool stressed = newDrops || fill >= cfg_.stressQueueFraction ||
                          (growing && fill >= cfg_.growingQueueFraction);
    const bool healthy = !stressed && !newDrops && fill <= cfg_.healthyQueueFraction;

    if (stressed) {
        ++stressTicks_;
        healthySinceMs_ = -1;
    } else {
        stressTicks_ = 0;
        if (healthy) {
            if (healthySinceMs_ < 0) healthySinceMs_ = nowMs;
        } else {
            healthySinceMs_ = -1;   // neutral: not stressed, but not recovered either
        }
    }

    const bool downCooled = !changedOnce_ || nowMs - lastChangeMs_ >= cfg_.downCooldownMs;
    if (stressTicks_ >= cfg_.stressTicksToDown && downCooled && cur_ > min_) {
        const int next = std::max(min_, static_cast<int>(std::floor(cur_ * cfg_.stepDown)));
        stressTicks_ = 0;
        if (next != cur_) {
            cur_ = next;
            lastChangeMs_ = nowMs;
            changedOnce_ = true;
            return cur_;
        }
        return 0;
    }

    const bool upCooled = !changedOnce_ || nowMs - lastChangeMs_ >= cfg_.upCooldownMs;
    if (healthySinceMs_ >= 0 && nowMs - healthySinceMs_ >= cfg_.healthyMsToUp && upCooled && cur_ < cfg_.targetKbps) {
        const int step = std::max(50, static_cast<int>(std::lround(cfg_.targetKbps * cfg_.stepUpFraction)));
        cur_ = std::min(cfg_.targetKbps, cur_ + step);
        lastChangeMs_ = nowMs;
        changedOnce_ = true;
        healthySinceMs_ = nowMs;   // the next step needs another full healthy period: slow recovery
        return cur_;
    }
    return 0;
}

}  // namespace hb::streaming
