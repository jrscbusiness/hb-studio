#pragma once
// QualityGovernor: automatic quality steps for the UI PREVIEW only.
//
// It watches the health of the Program clock (measured fps against the target, and ticks the clock had to
// skip) and lowers the preview quality preset one step at a time when the clock struggles; after a sustained
// period of health it climbs back up, never above the preset the operator picked. It never touches the Program
// output resolution: it only returns a preview preset index, the host applies it (Engine::setPreviewQuality).
//
// Pure logic, no threads of its own, no clock dependency: update() takes the sample and the current time in
// seconds (an injectable clock for tests; the overload without a time uses steady_clock). All methods are
// thread safe (the feeder thread calls update(), the UI message thread calls setUserLevel()/setEnabled()).
//
// Anti-flapping:
//   * a step down needs `badSamplesToStepDown` consecutive bad samples and `stepDownCooldownSec` since the last
//     change; a step up needs `goodSamplesToStepUp` consecutive good samples and `stepUpCooldownSec`;
//   * the band between "bad" and "good" (fps between 90% and 97% of the target, no skips) resets both streaks;
//   * if trouble returns shortly after a step up (within `flapWindowSec`) the sustained-health requirement is
//     doubled (up to 8x) until things have been calm for `penaltyResetSec`.
#include <cstdint>
#include <functional>
#include <mutex>

namespace hb::perf {

struct GovernorConfig {
    double minSampleSec = 1.0;            // evaluation period: update() calls closer together are ignored
    double badFpsFraction = 0.90;         // fps below this fraction of the target counts as a bad sample
    double goodFpsFraction = 0.97;        // fps at/above this (and no skipped ticks) counts as a good sample
    std::uint64_t badSkipsPerSample = 1;  // this many newly skipped clock ticks in a sample is a bad sample
    int badSamplesToStepDown = 2;
    int goodSamplesToStepUp = 10;
    double stepDownCooldownSec = 3.0;
    double stepUpCooldownSec = 15.0;
    double flapWindowSec = 45.0;
    double penaltyResetSec = 180.0;
};

struct GovernorSample {
    double fps = 0;                  // measured Program fps (0 while the clock is not running)
    std::uint64_t skipped = 0;       // cumulative skipped clock ticks
    int targetFps = 30;
};

class QualityGovernor {
public:
    using Clock = std::function<double()>;   // seconds, monotonic
    // `levels` = number of presets (0 = lightest); `userLevel` = the operator's choice (the ceiling).
    QualityGovernor(GovernorConfig cfg, int levels, int userLevel, Clock clock = nullptr);

    void setEnabled(bool on);
    bool enabled() const;
    // The operator picked a preset: it becomes the ceiling and is applied immediately.
    void setUserLevel(int level);
    int userLevel() const;

    // Feeds one sample; returns the preview level that should be active now.
    int update(const GovernorSample& s);
    int update(const GovernorSample& s, double nowSec);

    int level() const;          // currently chosen level
    bool reduced() const;       // level() is below the operator's choice
    int stepsDown() const;      // how many times it lowered the quality since construction (diagnostics)

private:
    GovernorConfig cfg_;
    int levels_;
    Clock clock_;
    mutable std::mutex mutex_;
    bool enabled_ = true;
    int user_ = 0, level_ = 0;
    bool haveSample_ = false;
    std::uint64_t lastSkipped_ = 0;
    double lastEvalSec_ = 0, lastChangeSec_ = 0, lastUpSec_ = -1e9, lastTroubleSec_ = -1e9;
    int badStreak_ = 0, goodStreak_ = 0;
    int penalty_ = 1;
    int stepsDown_ = 0;
};

}  // namespace hb::perf
