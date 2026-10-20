#include "audio/drift_compensator.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace hb::audio {

DriftCompensator::DriftCompensator(int inRate, int channels, std::size_t targetFillFrames,
                                   ResamplerQuality q, DriftConfig cfg)
    : rs_(inRate, kProgramSampleRate, channels, q),
      cfg_(cfg),
      channels_(channels),
      inRate_(inRate),
      target_(targetFillFrames) {}

void DriftCompensator::reset() noexcept {
    rs_.reset();
    rs_.setRatioTrimPpm(0);
    smoothed_ = integral_ = ppm_ = 0;
    primed_ = haveSmoothed_ = false;
}

double DriftCompensator::update(double fillFrames, double dt) noexcept {
    if (!haveSmoothed_) {
        smoothed_ = fillFrames;
        haveSmoothed_ = true;
    } else {
        const double a = 1.0 - std::exp(-dt / std::max(1e-3, cfg_.smoothingSeconds));
        smoothed_ += a * (fillFrames - smoothed_);
    }
    const double err = smoothed_ - static_cast<double>(target_);
    integral_ += err * dt;
    // Anti-windup: the integral term alone may not exceed the ppm clamp.
    const double iLimit = cfg_.maxPpm / std::max(1e-9, cfg_.ki);
    integral_ = std::clamp(integral_, -iLimit, iLimit);
    ppm_ = std::clamp(cfg_.kp * err + cfg_.ki * integral_, -cfg_.maxPpm, cfg_.maxPpm);
    rs_.setRatioTrimPpm(ppm_);
    return ppm_;
}

std::size_t DriftCompensator::pull(RingBuffer& ring, float* out, std::size_t frames) noexcept {
    const std::size_t ch = static_cast<std::size_t>(channels_);
    if (!primed_) {
        if (ring.size() / ch < target_) {
            std::memset(out, 0, frames * ch * sizeof(float));
            return frames;
        }
        primed_ = true;
    }
    update(static_cast<double>(ring.size() / ch) + rs_.pendingInputFrames(),
           static_cast<double>(frames) / kProgramSampleRate);
    rs_.pull([&](float* dst, std::size_t n) { ring.readZeroFill(dst, n * ch); }, out, frames);
    return frames;
}

}  // namespace hb::audio
