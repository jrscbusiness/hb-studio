#pragma once
// CaptureStream: opens an input (microphone, line, or WASAPI loopback of the system output) at the
// device's NATIVE rate and pushes float32 stereo into an SPSC ring buffer (producer = OS audio
// thread, consumer = the engine thread). Only the channel layout is converted (to stereo).
//
// PlaybackStream: monitoring output (headphones). The producer writes 48 kHz stereo; the device
// callback resamples to the device's native rate with a PI drift controller driven by the ring
// fill level (same idea as hb::audio::DriftCompensator, here in the opposite direction).
#include <atomic>
#include <memory>
#include <mutex>

#include "audio/resampler.hpp"
#include "audio/ring_buffer.hpp"
#include "audio_io/device_stream.hpp"

namespace hb::audio_io {

// One open() of a capture device. A new session (new ring, maybe new rate) is created on every
// (re)open, so stale audio of a previous device never leaks into a new one.
struct CaptureSession {
    CaptureSession(int rate, std::size_t ringFrames, std::uint64_t gen)
        : ring(ringFrames * 2), sampleRate(rate), generation(gen) {}
    hb::audio::RingBuffer ring;  // interleaved stereo at sampleRate
    int sampleRate;
    std::uint64_t generation;
};

class CaptureStream final : public DeviceStream {
public:
    // kind must be Capture or Loopback (anything else is treated as Capture).
    CaptureStream(std::string deviceId, DeviceKind kind = DeviceKind::Capture, StreamOptions options = {},
                  int ringMs = 500);
    ~CaptureStream() override { shutdown(); }

    // Latest session (nullptr before the first successful open). Thread-safe.
    std::shared_ptr<CaptureSession> session() const;

protected:
    RawCallbacks onOpened(int sampleRate) override;
    void fillCounters(StreamStatus& s) const override;

private:
    int ringMs_;
    mutable std::mutex sessionMutex_;
    std::shared_ptr<CaptureSession> session_;
    std::uint64_t generation_ = 0;
};

class PlaybackStream final : public DeviceStream {
public:
    PlaybackStream(std::string deviceId, StreamOptions options = {}, int latencyMs = 40, int ringMs = 500);
    ~PlaybackStream() override;

    // Producer side: 48 kHz interleaved stereo. Lock free; frames are dropped silently while the
    // device is not open (so an unplugged headset does not count as ring overruns).
    void write(const float* stereo48k, std::size_t frames) noexcept;
    double driftPpm() const noexcept { return ppm_.load(std::memory_order_relaxed); }

protected:
    RawCallbacks onOpened(int sampleRate) override;
    void onClosed() override { open_.store(false, std::memory_order_release); }
    void fillCounters(StreamStatus& s) const override;

private:
    struct Render;  // device-thread state (resampler, PI controller)
    void render(Render& r, float* out, std::size_t frames) noexcept;

    std::size_t targetFrames_;
    hb::audio::RingBuffer ring_;
    std::unique_ptr<Render> render_;
    std::atomic<bool> open_{false};
    std::atomic<double> ppm_{0.0};
};

}  // namespace hb::audio_io
