#pragma once
// Level meters for interleaved stereo float32. Not thread-safe: each meter is
// owned and fed by the audio thread (the Mixer publishes snapshots through
// atomics). None of them allocate after construction.
#include <array>
#include <cstddef>
#include <vector>

#include "audio/audio_common.hpp"
#include "audio/biquad.hpp"

namespace hb::audio {

// ---- Sample peak with hold and decay -------------------------------------
struct PeakMeterConfig {
    int sampleRate = kProgramSampleRate;
    double holdSeconds = 1.5;
    double decayDbPerSecond = 20.0;
};

class PeakMeter {
public:
    explicit PeakMeter(PeakMeterConfig cfg = {});
    void process(const float* interleaved, std::size_t frames) noexcept;
    // Feed pre-computed block peaks (linear, >= 0) covering `frames` frames.
    void update(float peakL, float peakR, std::size_t frames) noexcept;
    // Ballistic level: instant attack, linear-in-dB decay.
    float levelDb(int ch) const noexcept { return level_[ch]; }
    // Peak-hold marker: sticks to the maximum for holdSeconds, then decays.
    float holdDb(int ch) const noexcept { return hold_[ch]; }
    void reset() noexcept;

private:
    PeakMeterConfig cfg_;
    std::array<float, 2> level_{kNegInfDb, kNegInfDb};
    std::array<float, 2> hold_{kNegInfDb, kNegInfDb};
    std::array<double, 2> holdTimer_{0, 0};
};

// ---- Sliding-window RMS ----------------------------------------------------
class RmsMeter {
public:
    explicit RmsMeter(int sampleRate = kProgramSampleRate, double windowSeconds = 0.3);
    void process(const float* interleaved, std::size_t frames) noexcept;
    float rmsDb(int ch) const noexcept;  // dBFS (a full-scale sine reads -3.01)
    void reset() noexcept;

private:
    std::size_t n_;
    std::array<std::vector<double>, 2> sq_;
    std::array<double, 2> sum_{0, 0};
    std::size_t pos_ = 0;
};

// ---- True Peak (ITU-R BS.1770-4 Annex 2, 4x oversampling) ------------------
class TruePeakMeter {
public:
    TruePeakMeter() { reset(); }
    void process(const float* interleaved, std::size_t frames) noexcept;
    // Maximum true peak since reset(), in dBTP.
    float maxDb(int ch) const noexcept;
    void reset() noexcept;

private:
    static constexpr int kTaps = 12;
    std::array<std::array<float, 2 * kTaps>, 2> hist_{};  // doubled for branch-free windowing
    std::array<int, 2> pos_{0, 0};
    std::array<float, 2> max_{0, 0};
};

// ---- K-weighting (BS.1770) -------------------------------------------------
struct KWeightCoeffs {
    BiquadCoeffs shelf;      // stage 1: high-shelf (head effects)
    BiquadCoeffs highpass;   // stage 2: RLB high-pass
};
// 48 kHz returns the exact coefficients tabulated in BS.1770-4; other rates
// derive them from the analogue prototype.
KWeightCoeffs designKWeighting(double sampleRate);

// ---- Momentary / short-term loudness (LUFS) -------------------------------
// Energy is accumulated in 100 ms steps; momentary = last 400 ms (4 steps),
// short-term = last 3 s (30 steps). Until the window is full, the available
// steps are averaged. Silence (or no data) reads -inf.
class LoudnessMeter {
public:
    explicit LoudnessMeter(int sampleRate = kProgramSampleRate);
    void process(const float* interleaved, std::size_t frames) noexcept;
    float momentaryLufs() const noexcept { return window(4); }
    float shortTermLufs() const noexcept { return window(30); }
    void reset() noexcept;

private:
    float window(std::size_t steps) const noexcept;

    static constexpr std::size_t kSteps = 30;
    std::size_t stepFrames_;
    std::array<Biquad, 2> shelf_, hp_;
    std::array<double, 2> acc_{0, 0};
    std::size_t accFrames_ = 0;
    std::array<std::array<double, kSteps>, 2> ring_{};  // mean square per step
    std::size_t head_ = 0, count_ = 0;
};

}  // namespace hb::audio
