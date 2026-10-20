#pragma once
// Per-source delay for lip-sync offset (0..maxDelayMs, integer-sample taps).
// Delay changes are applied with a short equal-gain crossfade between the old
// and new tap, so there are no clicks. setDelayMs() may be called from any
// thread; process() is real-time safe and runs on the audio thread.
#include <atomic>
#include <cstddef>
#include <vector>

#include "audio/audio_common.hpp"

namespace hb::audio {

class DelayLine {
public:
    explicit DelayLine(int sampleRate = kProgramSampleRate, int channels = 2,
                       double maxDelayMs = 2000.0, double crossfadeMs = 20.0);

    void setDelayMs(double ms) noexcept;  // clamped to [0, maxDelayMs]
    double delayMs() const noexcept;
    double maxDelayMs() const noexcept { return maxDelayMs_; }

    // In-place processing of interleaved frames.
    void process(float* inout, std::size_t frames) noexcept;
    void reset() noexcept;

private:
    int sampleRate_, channels_;
    double maxDelayMs_;
    std::size_t maxDelaySamples_, capacity_, fadeLen_;
    std::vector<float> buf_;  // capacity_ frames, interleaved
    std::size_t w_ = 0;
    std::size_t curDelay_ = 0, prevDelay_ = 0, fadePos_ = 0;
    bool fading_ = false;
    std::atomic<std::size_t> target_{0};
};

}  // namespace hb::audio
