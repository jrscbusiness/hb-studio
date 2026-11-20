#include "audio_io/streams.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "audio/audio_common.hpp"

namespace hb::audio_io {

// ------------------------------------------------------------------ CaptureStream
CaptureStream::CaptureStream(std::string deviceId, DeviceKind kind, StreamOptions options, int ringMs)
    : DeviceStream(kind == DeviceKind::Loopback ? DeviceKind::Loopback : DeviceKind::Capture, std::move(deviceId),
                   std::move(options)),
      ringMs_(std::clamp(ringMs, 100, 5000)) {}

std::shared_ptr<CaptureSession> CaptureStream::session() const {
    std::lock_guard lock(sessionMutex_);
    return session_;
}

RawCallbacks CaptureStream::onOpened(int sampleRate) {
    const std::size_t ringFrames = static_cast<std::size_t>(sampleRate) * static_cast<std::size_t>(ringMs_) / 1000;
    auto s = std::make_shared<CaptureSession>(sampleRate, ringFrames, ++generation_);
    {
        std::lock_guard lock(sessionMutex_);
        session_ = s;
    }
    CaptureSession* raw = s.get();  // kept alive by session_ until the next open (device is closed by then)
    RawCallbacks cb;
    cb.capture = [this, raw](const float* data, std::size_t frames) {
        raw->ring.write(data, frames * 2);
        noteData(frames);
    };
    return cb;
}

void CaptureStream::fillCounters(StreamStatus& st) const {
    auto s = session();
    if (!s) return;
    st.underruns = s->ring.underruns();
    st.overruns = s->ring.overruns();
}

// ------------------------------------------------------------------ PlaybackStream
struct PlaybackStream::Render {
    Render(int rate, std::size_t target)
        : rs(hb::audio::kProgramSampleRate, rate, 2, hb::audio::ResamplerQuality::Medium), deviceRate(rate),
          targetFill(target) {}
    hb::audio::Resampler rs;
    int deviceRate;
    std::size_t targetFill;
    bool primed = false;
    double smoothed = 0, integral = 0, ppm = 0;
    bool haveSmoothed = false;
};

PlaybackStream::PlaybackStream(std::string deviceId, StreamOptions options, int latencyMs, int ringMs)
    : DeviceStream(DeviceKind::Playback, std::move(deviceId), std::move(options)),
      targetFrames_(static_cast<std::size_t>(std::clamp(latencyMs, 10, 400)) * hb::audio::kProgramSampleRate / 1000),
      ring_(static_cast<std::size_t>(std::clamp(ringMs, 100, 2000)) * hb::audio::kProgramSampleRate / 1000 * 2) {}

PlaybackStream::~PlaybackStream() { shutdown(); }

void PlaybackStream::write(const float* stereo48k, std::size_t frames) noexcept {
    if (!open_.load(std::memory_order_acquire) || !stereo48k) return;
    ring_.write(stereo48k, frames * 2);
}

RawCallbacks PlaybackStream::onOpened(int sampleRate) {
    render_ = std::make_unique<Render>(sampleRate, targetFrames_);
    open_.store(true, std::memory_order_release);
    Render* r = render_.get();
    RawCallbacks cb;
    cb.playback = [this, r](float* out, std::size_t frames) {
        render(*r, out, frames);
        noteData(frames);
    };
    return cb;
}

void PlaybackStream::render(Render& r, float* out, std::size_t frames) noexcept {
    constexpr std::size_t ch = 2;
    if (!r.primed) {
        if (ring_.size() / ch < r.targetFill) {
            std::memset(out, 0, frames * ch * sizeof(float));
            return;
        }
        r.primed = true;
        // Discard what accumulated while the device was closed / priming beyond the target.
        float scratch[256 * ch];
        while (ring_.size() / ch > r.targetFill) {
            const std::size_t n = std::min<std::size_t>(256, ring_.size() / ch - r.targetFill);
            ring_.read(scratch, n * ch);
        }
    }
    const double dt = static_cast<double>(frames) / r.deviceRate;
    const double fill = static_cast<double>(ring_.size() / ch) + r.rs.pendingInputFrames();
    if (!r.haveSmoothed) {
        r.smoothed = fill;
        r.haveSmoothed = true;
    } else {
        r.smoothed += (1.0 - std::exp(-dt / 1.0)) * (fill - r.smoothed);
    }
    constexpr double kp = 2.0, ki = 0.1, maxPpm = 500.0;
    const double err = r.smoothed - static_cast<double>(r.targetFill);
    r.integral = std::clamp(r.integral + err * dt, -maxPpm / ki, maxPpm / ki);
    r.ppm = std::clamp(kp * err + ki * r.integral, -maxPpm, maxPpm);
    r.rs.setRatioTrimPpm(r.ppm);
    ppm_.store(r.ppm, std::memory_order_relaxed);
    r.rs.pull([&](float* dst, std::size_t n) { ring_.readZeroFill(dst, n * ch); }, out, frames);
}

void PlaybackStream::fillCounters(StreamStatus& st) const {
    st.underruns = ring_.underruns();
    st.overruns = ring_.overruns();
}

}  // namespace hb::audio_io
