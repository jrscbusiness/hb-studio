#pragma once
// Hand-off between the audio engine thread and the caption worker.
//   AudioTapQueue: bounded ring of interleaved stereo 48 kHz floats, drop-OLDEST when full. push() is
//     called from the engine thread: it takes a very short mutex, copies, never allocates and never
//     waits for the consumer (a slow recogniser can therefore never glitch the audio engine).
//   ProgramDownmixer: stereo 48 kHz -> mono 16 kHz (hb::audio::Resampler, 3:1, low-pass inside).
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "audio/resampler.hpp"

namespace hb::captions {

constexpr int kProgramRate = 48000;

class AudioTapQueue {
public:
    explicit AudioTapQueue(std::size_t capacityFrames);
    // Engine thread. Never blocks beyond the brief lock. Oversized blocks keep only their newest frames.
    void push(const float* stereo, std::size_t frames) noexcept;
    // Worker. Copies up to maxFrames; returns frames copied (0 if empty).
    std::size_t pop(float* out, std::size_t maxFrames);
    // Worker. Waits up to timeoutMs for data (or close()). Returns true if data is available.
    bool waitForData(int timeoutMs);
    void close();               // wakes waiters; later push() is ignored
    void reopen();
    void clear();
    std::size_t sizeFrames() const;
    std::uint64_t droppedFrames() const;

private:
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::vector<float> ring_;   // interleaved stereo
    std::size_t cap_, head_ = 0, count_ = 0;   // frames
    std::uint64_t dropped_ = 0;
    bool closed_ = false;
};

class ProgramDownmixer {
public:
    ProgramDownmixer();
    // Appends mono 16 kHz samples to `out`.
    void process(const float* stereo48k, std::size_t frames, std::vector<float>& out);
    void reset();

private:
    std::unique_ptr<hb::audio::Resampler> rs_;
    std::vector<float> mono_, res_;
};

}  // namespace hb::captions
