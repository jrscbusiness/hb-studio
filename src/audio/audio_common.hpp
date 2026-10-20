#pragma once
// Shared constants, dB helpers and the AudioSource interface.
// All program audio is interleaved stereo float32 at 48 kHz unless stated.
#include <cmath>
#include <cstddef>
#include <limits>

namespace hb::audio {

inline constexpr int kProgramSampleRate = 48000;
inline constexpr int kChannels = 2;  // stereo everywhere in the program path
inline constexpr double kPi = 3.14159265358979323846;
inline constexpr float kNegInfDb = -std::numeric_limits<float>::infinity();

inline float dbToLinear(float db) noexcept {
    if (std::isinf(db) && db < 0.0f) return 0.0f;
    return std::pow(10.0f, db / 20.0f);
}
inline float linearToDb(float lin) noexcept {
    return lin <= 0.0f ? kNegInfDb : 20.0f * std::log10(lin);
}

// Pull-style audio producer. Camera inputs (RTSP/NDI/UVC) implement this later.
class AudioSource {
public:
    virtual ~AudioSource() = default;
    virtual int sampleRate() const noexcept { return kProgramSampleRate; }
    // Fills `frames` interleaved stereo frames (always the whole buffer: the
    // part that is not real content is zeroed). Returns the number of frames
    // that carried real content. Called from the audio thread: no allocation,
    // no locks.
    virtual std::size_t pull(float* interleavedStereo, std::size_t frames) noexcept = 0;
};

}  // namespace hb::audio
