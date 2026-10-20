#include "audio/sources.hpp"

#include <cstring>

namespace hb::audio {

std::size_t SilenceSource::pull(float* out, std::size_t frames) noexcept {
    std::memset(out, 0, frames * kChannels * sizeof(float));
    return frames;
}

ToneSource::ToneSource(double freqHz, float peakDbfs, int sampleRate)
    : sampleRate_(sampleRate), freq_(freqHz), amp_(dbToLinear(peakDbfs)), lastAmp_(dbToLinear(peakDbfs)) {}

std::size_t ToneSource::pull(float* out, std::size_t frames) noexcept {
    const double inc = freq_.load(std::memory_order_relaxed) / sampleRate_;
    const float target = amp_.load(std::memory_order_relaxed);
    const float step = frames ? (target - lastAmp_) / static_cast<float>(frames) : 0.0f;
    float a = lastAmp_;
    for (std::size_t i = 0; i < frames; ++i) {
        a += step;
        const float s = a * static_cast<float>(std::sin(2.0 * kPi * phase_));
        out[2 * i] = s;
        out[2 * i + 1] = s;
        phase_ += inc;
        if (phase_ >= 1.0) phase_ -= 1.0;
    }
    lastAmp_ = target;
    return frames;
}

double PinkNoiseSource::Channel::next() noexcept {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    const double w = static_cast<double>(rng) / 2147483648.0 - 1.0;  // [-1, 1)
    b0 = 0.99886 * b0 + w * 0.0555179;
    b1 = 0.99332 * b1 + w * 0.0750759;
    b2 = 0.96900 * b2 + w * 0.1538520;
    b3 = 0.86650 * b3 + w * 0.3104856;
    b4 = 0.55000 * b4 + w * 0.5329522;
    b5 = -0.7616 * b5 - w * 0.0168980;
    const double pink = b0 + b1 + b2 + b3 + b4 + b5 + b6 + w * 0.5362;
    b6 = w * 0.115926;
    return pink;
}

PinkNoiseSource::PinkNoiseSource(float rmsDbfs, std::uint32_t seed, int sampleRate)
    : sampleRate_(sampleRate), gain_(dbToLinear(rmsDbfs) * kUnitRms) {
    if (seed == 0) seed = 1;
    ch_[0].rng = seed * 2654435761u | 1u;
    ch_[1].rng = (seed + 0x9E3779B9u) * 2246822519u | 1u;
}

std::size_t PinkNoiseSource::pull(float* out, std::size_t frames) noexcept {
    const float g = gain_.load(std::memory_order_relaxed);
    for (std::size_t i = 0; i < frames; ++i) {
        out[2 * i] = g * static_cast<float>(ch_[0].next());
        out[2 * i + 1] = g * static_cast<float>(ch_[1].next());
    }
    return frames;
}

}  // namespace hb::audio
