#pragma once
// Adaptive resampling to absorb clock drift between a source (e.g. a camera
// whose audio clock differs from the Program clock) and the 48 kHz Program
// bus. A PI controller turns the ring-buffer fill level into a ratio trim in
// ppm: buffer above target -> consume input slightly faster, below -> slower.
// Runs entirely on the consumer (audio) thread.
#include <cstddef>

#include "audio/audio_common.hpp"
#include "audio/resampler.hpp"
#include "audio/ring_buffer.hpp"

namespace hb::audio {

struct DriftConfig {
    double maxPpm = 500.0;          // clamp of the correction
    double kp = 2.0;                // ppm per frame of fill error
    double ki = 0.1;                // ppm per frame*second of accumulated error
    double smoothingSeconds = 1.0;  // low-pass on the measured fill level
};

class DriftCompensator {
public:
    // targetFillFrames: desired backlog (input-rate frames) measured at the
    // start of each pull() = latency budget; it includes the ring fill plus
    // what the resampler already holds.
    DriftCompensator(int inRate, int channels, std::size_t targetFillFrames,
                     ResamplerQuality q = ResamplerQuality::Medium, DriftConfig cfg = {});

    // Controller step: `fillFrames` is the current ring fill in input frames,
    // `dtSeconds` the time since the previous call. Returns the new trim (ppm).
    double update(double fillFrames, double dtSeconds) noexcept;

    // Pulls `frames` 48 kHz output frames from `ring` (interleaved, `channels`
    // wide, input-rate content). Waits (outputs silence) until the ring first
    // reaches the target fill (priming). Underruns are zero-filled and counted
    // by the ring. Returns frames written (always `frames`).
    std::size_t pull(RingBuffer& ring, float* out, std::size_t frames) noexcept;

    double currentPpm() const noexcept { return ppm_; }
    bool primed() const noexcept { return primed_; }
    void reset() noexcept;

private:
    Resampler rs_;
    DriftConfig cfg_;
    int channels_;
    int inRate_;
    std::size_t target_;
    double smoothed_ = 0, integral_ = 0, ppm_ = 0;
    bool primed_ = false, haveSmoothed_ = false;
};

}  // namespace hb::audio
