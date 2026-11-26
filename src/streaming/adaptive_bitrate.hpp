#pragma once
#include <cstdint>

// Pure decision logic for adaptive bitrate (opt-in per destination, default OFF). Fed one sample per
// tick (~1 s) with the send queue's depth and the cumulative drop count; answers "ask the encoder for
// a new bitrate now?". No clock, threads or encoder in here: the caller passes `nowMs` and applies the
// result (StreamSender -> IH264Encoder::setBitrateKbps, best effort), which is what makes the
// hysteresis/cooldown behaviour unit-testable.
//
// Behaviour: step DOWN (x stepDown, floor = minFraction of target) after `stressTicksToDown`
// consecutive stressed samples and a cooldown since the last change; step UP slowly (+stepUpFraction
// of target) only after the queue stayed healthy for `healthyMsToUp` and a cooldown. Mixed samples
// ("neutral") neither build stress nor count as healthy, so the controller does not flap.
namespace hb::streaming {

struct AbrConfig {
    int targetKbps = 6000;
    double minFraction = 0.4;          // never below 40% of the target
    double stepDown = 0.85;            // multiply on each reduction
    double stepUpFraction = 0.05;      // add this fraction of the target on each recovery step
    int stressTicksToDown = 3;
    int downCooldownMs = 5000;
    int healthyMsToUp = 20000;
    int upCooldownMs = 10000;
    double stressQueueFraction = 0.5;  // queue >= this share of capacity = stressed
    double growingQueueFraction = 0.25;// queue >= this share AND larger than last tick = stressed
    double healthyQueueFraction = 0.15;// queue <= this share and no drops = healthy
};

struct AbrSample {
    int queueDepth = 0;
    int queueCapacity = 1;
    std::uint64_t droppedTotal = 0;    // cumulative frames dropped by the bounded queue
};

class AdaptiveBitrate {
public:
    explicit AdaptiveBitrate(const AbrConfig& cfg = {});
    int targetKbps() const { return cfg_.targetKbps; }
    int minKbps() const { return min_; }
    int currentKbps() const { return cur_; }
    // Returns the new bitrate when it should change now, else 0.
    int update(std::int64_t nowMs, const AbrSample& s);
    void reset();

private:
    AbrConfig cfg_;
    int min_ = 0;
    int cur_ = 0;
    int stressTicks_ = 0;
    std::int64_t lastChangeMs_ = 0;
    bool changedOnce_ = false;
    std::int64_t healthySinceMs_ = -1;
    int prevDepth_ = 0;
    std::uint64_t prevDropped_ = 0;
    bool haveDropBaseline_ = false;
};

}  // namespace hb::streaming
