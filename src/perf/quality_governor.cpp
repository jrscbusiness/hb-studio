#include "perf/quality_governor.hpp"

#include <algorithm>
#include <chrono>

namespace hb::perf {

namespace {
double steadyNowSec() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
}  // namespace

QualityGovernor::QualityGovernor(GovernorConfig cfg, int levels, int userLevel, Clock clock)
    : cfg_(cfg), levels_(std::max(1, levels)), clock_(clock ? std::move(clock) : Clock(steadyNowSec)) {
    user_ = level_ = std::clamp(userLevel, 0, levels_ - 1);
}

void QualityGovernor::setEnabled(bool on) {
    std::lock_guard lock(mutex_);
    if (on == enabled_) return;
    enabled_ = on;
    level_ = user_;   // off: the operator's own choice comes back at once; on: start from it
    haveSample_ = false;
    badStreak_ = goodStreak_ = 0;
}

bool QualityGovernor::enabled() const {
    std::lock_guard lock(mutex_);
    return enabled_;
}

void QualityGovernor::setUserLevel(int level) {
    std::lock_guard lock(mutex_);
    user_ = level_ = std::clamp(level, 0, levels_ - 1);
    badStreak_ = goodStreak_ = 0;
    lastChangeSec_ = lastEvalSec_;   // measuring a freshly applied preset: let it settle before judging
}

int QualityGovernor::userLevel() const {
    std::lock_guard lock(mutex_);
    return user_;
}

int QualityGovernor::level() const {
    std::lock_guard lock(mutex_);
    return level_;
}

bool QualityGovernor::reduced() const {
    std::lock_guard lock(mutex_);
    return enabled_ && level_ < user_;
}

int QualityGovernor::stepsDown() const {
    std::lock_guard lock(mutex_);
    return stepsDown_;
}

int QualityGovernor::update(const GovernorSample& s) { return update(s, clock_()); }

int QualityGovernor::update(const GovernorSample& s, double nowSec) {
    std::lock_guard lock(mutex_);
    if (!enabled_ || s.targetFps <= 0) return level_;
    if (!haveSample_) {   // first sample only records the baseline of the skipped-tick counter
        haveSample_ = true;
        lastSkipped_ = s.skipped;
        lastEvalSec_ = nowSec;
        lastChangeSec_ = nowSec;
        return level_;
    }
    if (nowSec - lastEvalSec_ < cfg_.minSampleSec) return level_;
    lastEvalSec_ = nowSec;
    const std::uint64_t newSkips = s.skipped >= lastSkipped_ ? s.skipped - lastSkipped_ : 0;
    lastSkipped_ = s.skipped;

    if (s.fps <= 0.0) {   // clock stopped / not measured yet: says nothing about load
        badStreak_ = goodStreak_ = 0;
        return level_;
    }
    const bool bad = s.fps < cfg_.badFpsFraction * s.targetFps || newSkips >= cfg_.badSkipsPerSample;
    const bool good = !bad && s.fps >= cfg_.goodFpsFraction * s.targetFps && newSkips == 0;
    if (bad) {
        ++badStreak_;
        goodStreak_ = 0;
        lastTroubleSec_ = nowSec;
    } else if (good) {
        ++goodStreak_;
        badStreak_ = 0;
    } else {   // in the band between: neither confirms trouble nor health
        badStreak_ = goodStreak_ = 0;
    }
    if (penalty_ > 1 && nowSec - lastTroubleSec_ >= cfg_.penaltyResetSec) penalty_ = 1;

    if (badStreak_ >= cfg_.badSamplesToStepDown && level_ > 0 && nowSec - lastChangeSec_ >= cfg_.stepDownCooldownSec) {
        --level_;
        ++stepsDown_;
        if (nowSec - lastUpSec_ < cfg_.flapWindowSec) penalty_ = std::min(penalty_ * 2, 8);   // climbed too early
        lastChangeSec_ = nowSec;
        badStreak_ = goodStreak_ = 0;
    } else if (goodStreak_ >= cfg_.goodSamplesToStepUp * penalty_ && level_ < user_ &&
               nowSec - lastChangeSec_ >= cfg_.stepUpCooldownSec) {
        ++level_;
        lastUpSec_ = nowSec;
        lastChangeSec_ = nowSec;
        badStreak_ = goodStreak_ = 0;
    }
    return level_;
}

}  // namespace hb::perf
