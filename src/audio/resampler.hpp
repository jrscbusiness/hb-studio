#pragma once
// Streaming sample-rate converter (interleaved float, 1..8 channels).
//   Quality::Linear : 2-tap linear interpolation (cheap, audible aliasing).
//   Quality::Low/Medium/High : Kaiser-windowed sinc, 16/32/64 taps (scaled up
//     by 1/ratio when down-sampling so the anti-alias transition stays narrow),
//     polyphase table with linear interpolation between phases.
// The output at index n is the signal at input time n * step (step =
// inRate/outRate, trimmable by ppm for drift compensation), i.e. no delay in
// the signal itself; the algorithmic look-ahead is latencyFrames() input frames.
// Not thread-safe; real-time safe after construction (no allocation).
#include <algorithm>
#include <cstddef>
#include <vector>

namespace hb::audio {

enum class ResamplerQuality { Linear, Low, Medium, High };

class Resampler {
public:
    Resampler(double inRate, double outRate, int channels = 2,
              ResamplerQuality q = ResamplerQuality::Medium, std::size_t maxChunkFrames = 4096);

    // Upper bound of output frames process() can produce for inFrames input.
    std::size_t maxOutputFrames(std::size_t inFrames) const noexcept;

    // Push mode: consumes all input, writes output (outCap must be >=
    // maxOutputFrames(inFrames)). Returns output frames written.
    std::size_t process(const float* in, std::size_t inFrames, float* out, std::size_t outCap) noexcept;

    // Pull mode: produces exactly `frames` output frames, calling
    // fetch(float* dst, size_t n) to obtain n more input frames when needed.
    template <class Fetch>
    std::size_t pull(Fetch&& fetch, float* out, std::size_t frames) noexcept {
        std::size_t done = 0;
        while (done < frames) {
            done += produce(out + done * channels_, frames - done);
            if (done >= frames) break;
            const std::size_t n = std::min(kPullChunk, capacityFrames_ - len_);
            float* dst = buf_.data() + len_ * channels_;
            fetch(dst, n);
            len_ += n;
        }
        return done;
    }

    // Fine ratio trim in parts per million; positive consumes input faster.
    void setRatioTrimPpm(double ppm) noexcept;
    double ratioTrimPpm() const noexcept { return trimPpm_; }
    void reset() noexcept;

    std::size_t latencyFrames() const noexcept { return static_cast<std::size_t>(half_); }
    // Input frames already held internally beyond the kernel look-ahead
    // (fractional, may be slightly negative). Lets a controller see the true
    // backlog independent of how pull() chunks its fetches.
    double pendingInputFrames() const noexcept { return static_cast<double>(len_) - pos_ - half_; }
    int taps() const noexcept { return taps_; }
    double nominalStep() const noexcept { return nominalStep_; }
    double inRate() const noexcept { return inRate_; }
    double outRate() const noexcept { return outRate_; }

private:
    static constexpr std::size_t kPullChunk = 64;

    void appendInput(const float* in, std::size_t n) noexcept;
    std::size_t produce(float* out, std::size_t cap) noexcept;  // also compacts

    double inRate_, outRate_, nominalStep_, step_;
    double trimPpm_ = 0;
    int channels_;
    bool linear_;
    int taps_ = 2, half_ = 1, phases_ = 1;
    std::vector<float> table_;  // (phases_+1) rows of taps_ weights
    std::vector<float> buf_;    // interleaved history + pending input
    std::size_t len_ = 0, capacityFrames_ = 0, maxChunk_;
    double pos_ = 0;
};

}  // namespace hb::audio
