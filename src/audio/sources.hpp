#pragma once
// Deterministic test sources (no audio device involved).
#include <atomic>
#include <cstdint>

#include "audio/audio_common.hpp"

namespace hb::audio {

class SilenceSource final : public AudioSource {
public:
    std::size_t pull(float* out, std::size_t frames) noexcept override;
};

// Sine, identical on both channels, phase-continuous. Frequency/level may be
// changed from another thread; the level change is ramped over the block.
class ToneSource final : public AudioSource {
public:
    explicit ToneSource(double freqHz = 1000.0, float peakDbfs = -20.0f, int sampleRate = kProgramSampleRate);
    void setFrequency(double hz) noexcept { freq_.store(hz, std::memory_order_relaxed); }
    void setLevelDb(float peakDbfs) noexcept { amp_.store(dbToLinear(peakDbfs), std::memory_order_relaxed); }
    int sampleRate() const noexcept override { return sampleRate_; }
    std::size_t pull(float* out, std::size_t frames) noexcept override;

private:
    int sampleRate_;
    std::atomic<double> freq_;
    std::atomic<float> amp_;
    float lastAmp_;
    double phase_ = 0;  // cycles
};

// Pink noise (-3 dB/octave, Paul Kellet's refined filter), independent
// generators for L and R, seeded xorshift -> bit-exact reproducible.
class PinkNoiseSource final : public AudioSource {
public:
    explicit PinkNoiseSource(float rmsDbfs = -20.0f, std::uint32_t seed = 1, int sampleRate = kProgramSampleRate);
    void setLevelDb(float rmsDbfs) noexcept { gain_.store(dbToLinear(rmsDbfs) * kUnitRms, std::memory_order_relaxed); }
    int sampleRate() const noexcept override { return sampleRate_; }
    std::size_t pull(float* out, std::size_t frames) noexcept override;

    // Multiplier that makes the generator output 0 dBFS RMS (calibrated by test).
    static constexpr float kUnitRms = 0.5584f;

private:
    struct Channel {
        std::uint32_t rng;
        double b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0;
        double next() noexcept;
    };
    int sampleRate_;
    Channel ch_[2];
    std::atomic<float> gain_;
};

}  // namespace hb::audio
