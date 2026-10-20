#pragma once
// Program-bus protection against clipping.
//   Off      : passthrough (still delayed, so switching modes keeps latency constant).
//   SoftClip : transparent below the knee, tanh-shaped (C1-continuous) above,
//              asymptotically bounded by the ceiling. No gain-pumping.
//   Limiter  : stereo-linked look-ahead limiter. Required gain is min-filtered
//              over the look-ahead window, then averaged over it, so gain
//              reduction is fully ramped in before the peak arrives (no
//              overshoot, no zipper steps); release is a one-pole recovery.
// Mode and ceiling are atomics (UI thread); process() is real-time safe.
#include <atomic>
#include <cstddef>
#include <vector>

#include "audio/audio_common.hpp"

namespace hb::audio {

enum class LimiterMode { Off = 0, SoftClip = 1, Limiter = 2 };

struct LimiterConfig {
    LimiterMode mode = LimiterMode::Limiter;
    float ceilingDb = -1.0f;
    float softKneeDb = -6.0f;  // knee start, relative to the ceiling
    float releaseMs = 100.0f;
    float lookaheadMs = 1.0f;
};

class Limiter {
public:
    explicit Limiter(int sampleRate = kProgramSampleRate, LimiterConfig cfg = {});

    void setMode(LimiterMode m) noexcept { mode_.store(static_cast<int>(m), std::memory_order_relaxed); }
    LimiterMode mode() const noexcept { return static_cast<LimiterMode>(mode_.load(std::memory_order_relaxed)); }
    void setCeilingDb(float db) noexcept;
    float ceilingDb() const noexcept { return ceilingDb_.load(std::memory_order_relaxed); }

    std::size_t latencyFrames() const noexcept { return delay_; }
    float lastGainReductionDb() const noexcept { return lastGrDb_; }

    void process(float* interleavedStereo, std::size_t frames) noexcept;
    void reset() noexcept;

private:
    int sr_;
    float knee_;
    std::atomic<int> mode_;
    std::atomic<float> ceilingDb_;
    std::atomic<float> ceilingLin_;
    std::size_t n_;      // look-ahead window length
    std::size_t delay_;  // n_ - 1
    float relCoef_;
    std::vector<float> audio_;  // delay line, n_ frames
    std::vector<float> req_;    // last n_ required gains
    std::vector<float> avg_;    // last n_ min-filtered gains
    std::size_t w_ = 0;
    double sum_ = 0;
    float rel_ = 1.0f;
    float lastGrDb_ = 0.0f;
};

}  // namespace hb::audio
