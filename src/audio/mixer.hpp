#pragma once
// Program mixer: N stereo float32 inputs -> PROGRAM bus.
//
// Per channel: trim gain, fader (dB curve, -inf at the bottom, +10 dB max),
// mute, solo (solo-in-place: when any channel is soloed only soloed channels
// reach the bus) and constant-power pan (-3 dB at centre). The bus then goes
// through a master fader and the Limiter/SoftClip stage, and is metered
// (peak/RMS/True Peak/LUFS).
//
// Threading: every setter is safe from any (UI) thread and only stores
// atomics. process() runs on the audio thread, takes no locks, never
// allocates, and smooths all gain changes with a linear ramp (rampMs) so
// there is no zipper noise regardless of block size. Level getters read
// snapshots published (relaxed atomics) by process().
#include <array>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "audio/audio_common.hpp"
#include "audio/limiter.hpp"
#include "audio/meters.hpp"

namespace hb::audio {

struct MixerConfig {
    std::vector<std::string> channelNames;
    int sampleRate = kProgramSampleRate;
    std::size_t maxBlockFrames = 4096;  // internal chunking / scratch size
    float rampMs = 10.0f;
    LimiterConfig limiter;

    // CAM 1..cameras, MIC 1, MIC 2, SYSTEM.
    static MixerConfig broadcastDefault(int cameras = 4);
};

struct ChannelLevels {
    float peakDb[2];  // input level after trim gain, before fader/mute
    float holdDb[2];
};

struct ProgramLevels {
    float peakDb[2];
    float holdDb[2];
    float rmsDb[2];
    float truePeakDb[2];  // max since last resetMeters()
    float momentaryLufs;
    float shortTermLufs;
    float limiterReductionDb;  // <= 0
};

// Fader law. position in [0,1]: 0 -> -inf, 0.75 -> 0 dB, 1 -> +10 dB.
// Knots (dB): 0.10:-60, 0.25:-40, 0.50:-20, 0.75:0, 1.00:+10; linear in dB
// between knots, linear-in-gain fade to silence below 0.10.
float faderPositionToDb(float position) noexcept;
float dbToFaderPosition(float db) noexcept;
inline constexpr float kFaderMaxDb = 10.0f;

// Constant-power pan law: pan in [-1,1]; gains satisfy l*l + r*r == 1
// (-3.01 dB each at the centre).
void panGains(float pan, float& l, float& r) noexcept;

class Mixer {
public:
    explicit Mixer(MixerConfig cfg);
    ~Mixer();
    Mixer(const Mixer&) = delete;
    Mixer& operator=(const Mixer&) = delete;

    std::size_t channelCount() const noexcept { return count_; }
    const std::string& channelName(std::size_t ch) const { return names_[ch]; }

    // ---- control (any thread) ------------------------------------------
    void setGainDb(std::size_t ch, float db) noexcept;       // trim, -inf allowed
    void setFaderDb(std::size_t ch, float db) noexcept;      // clamped to <= +10, -inf allowed
    void setFaderPosition(std::size_t ch, float pos) noexcept;
    void setMute(std::size_t ch, bool on) noexcept;
    void setSolo(std::size_t ch, bool on) noexcept;
    void setPan(std::size_t ch, float pan) noexcept;         // -1 (L) .. +1 (R)
    void setMasterFaderDb(float db) noexcept;
    void setLimiterMode(LimiterMode m) noexcept { limiter_.setMode(m); }
    void setLimiterCeilingDb(float db) noexcept { limiter_.setCeilingDb(db); }
    void resetMeters() noexcept { resetMeters_.store(true, std::memory_order_relaxed); }

    float gainDb(std::size_t ch) const noexcept;
    float faderDb(std::size_t ch) const noexcept;
    bool muted(std::size_t ch) const noexcept;
    bool soloed(std::size_t ch) const noexcept;
    float pan(std::size_t ch) const noexcept;
    LimiterMode limiterMode() const noexcept { return limiter_.mode(); }

    // Non-owning; the source must outlive its use by process(). Set while
    // the audio thread is stopped, or accept that the swap is atomic.
    void attachSource(std::size_t ch, AudioSource* src) noexcept;

    // ---- audio thread --------------------------------------------------
    // inputs[ch] points at `frames` interleaved stereo frames (nullptr =
    // silence). Writes `frames` interleaved stereo frames to programOut.
    void process(const float* const* inputs, float* programOut, std::size_t frames) noexcept;
    // Same, pulling from the attached sources.
    void process(float* programOut, std::size_t frames) noexcept;

    // Latency added by the limiter look-ahead.
    std::size_t latencyFrames() const noexcept { return limiter_.latencyFrames(); }

    // ---- meters (any thread) ---------------------------------------------
    ChannelLevels channelLevels(std::size_t ch) const noexcept;
    ProgramLevels programLevels() const noexcept;

private:
    struct Ramp {
        float cur = 0, target = 0, step = 0;
    };
    struct Channel;

    template <class GetInput>
    void run(GetInput&& get, float* out, std::size_t frames) noexcept;
    void mixChannel(Channel& c, const float* in, float* out, std::size_t n, bool audible) noexcept;
    void publishProgram() noexcept;

    std::size_t count_;
    std::vector<std::string> names_;
    int sampleRate_;
    std::size_t maxBlock_;
    std::size_t rampSamples_;
    std::vector<std::unique_ptr<Channel>> chStorage_;
    std::vector<float> scratch_, zeros_;

    std::atomic<float> masterGainLin_{1.0f};
    std::atomic<float> masterDb_{0.0f};
    std::atomic<bool> resetMeters_{false};
    float masterCur_ = 1.0f, masterStep_ = 0.0f, masterTarget_ = 1.0f;
    std::size_t masterRemaining_ = 0;

    Limiter limiter_;
    PeakMeter programPeak_;
    RmsMeter programRms_;
    TruePeakMeter programTp_;
    LoudnessMeter programLoud_;
    std::array<std::atomic<float>, 11> prog_;
};

}  // namespace hb::audio
