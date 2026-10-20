#include "audio/resampler.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "audio/audio_common.hpp"

namespace hb::audio {

namespace {
double besselI0(double x) {
    double sum = 1.0, term = 1.0;
    const double q = x * x / 4.0;
    for (int k = 1; k < 60; ++k) {
        term *= q / (static_cast<double>(k) * k);
        sum += term;
        if (term < 1e-18 * sum) break;
    }
    return sum;
}
}  // namespace

Resampler::Resampler(double inRate, double outRate, int channels, ResamplerQuality q,
                     std::size_t maxChunkFrames)
    : inRate_(inRate),
      outRate_(outRate),
      nominalStep_(inRate / outRate),
      step_(inRate / outRate),
      channels_(std::clamp(channels, 1, 8)),
      linear_(q == ResamplerQuality::Linear),
      maxChunk_(std::max<std::size_t>(maxChunkFrames, kPullChunk)) {
    if (linear_) {
        taps_ = 2;
        half_ = 1;
        phases_ = 1;
    } else {
        int baseTaps = 32;
        double beta = 8.0;
        phases_ = 128;
        if (q == ResamplerQuality::Low) { baseTaps = 16; beta = 6.0; phases_ = 32; }
        if (q == ResamplerQuality::High) { baseTaps = 64; beta = 9.5; phases_ = 512; }
        const double ratio = outRate / inRate;  // < 1 when down-sampling
        double t = baseTaps;
        if (ratio < 1.0) t = std::ceil(baseTaps / ratio);
        taps_ = std::min(512, static_cast<int>(t) + (static_cast<int>(t) & 1));
        half_ = taps_ / 2;
        const double fc = 0.95 * std::min(1.0, ratio);  // relative to input Nyquist
        table_.assign(static_cast<std::size_t>(phases_ + 1) * taps_, 0.0f);
        const double i0b = besselI0(beta);
        for (int p = 0; p <= phases_; ++p) {
            const double f = static_cast<double>(p) / phases_;
            std::vector<double> row(taps_);
            double sum = 0;
            for (int k = 0; k < taps_; ++k) {
                const double tt = (k - (half_ - 1)) - f;
                const double x = fc * tt;
                const double sinc = std::fabs(x) < 1e-12 ? 1.0 : std::sin(kPi * x) / (kPi * x);
                const double r = tt / half_;
                const double w = std::fabs(r) >= 1.0 ? 0.0 : besselI0(beta * std::sqrt(1.0 - r * r)) / i0b;
                row[k] = fc * sinc * w;
                sum += row[k];
            }
            for (int k = 0; k < taps_; ++k)
                table_[static_cast<std::size_t>(p) * taps_ + k] = static_cast<float>(row[k] / sum);
        }
    }
    capacityFrames_ = static_cast<std::size_t>(taps_) + maxChunk_ + kPullChunk;
    buf_.assign(capacityFrames_ * channels_, 0.0f);
    reset();
}

void Resampler::reset() noexcept {
    std::fill(buf_.begin(), buf_.end(), 0.0f);
    len_ = static_cast<std::size_t>(half_ - 1);
    pos_ = static_cast<double>(half_ - 1);
}

void Resampler::setRatioTrimPpm(double ppm) noexcept {
    trimPpm_ = std::clamp(ppm, -10000.0, 10000.0);
    step_ = nominalStep_ * (1.0 + trimPpm_ * 1e-6);
}

std::size_t Resampler::maxOutputFrames(std::size_t inFrames) const noexcept {
    const double minStep = nominalStep_ * (1.0 - 0.01);
    return static_cast<std::size_t>(std::ceil(static_cast<double>(inFrames) / minStep)) + 4;
}

void Resampler::appendInput(const float* in, std::size_t n) noexcept {
    n = std::min(n, capacityFrames_ - len_);
    std::memcpy(buf_.data() + len_ * channels_, in, n * channels_ * sizeof(float));
    len_ += n;
}

std::size_t Resampler::process(const float* in, std::size_t inFrames, float* out,
                               std::size_t outCap) noexcept {
    std::size_t produced = 0;
    while (inFrames > 0) {
        const std::size_t n = std::min(inFrames, maxChunk_);
        appendInput(in, n);
        in += n * channels_;
        inFrames -= n;
        produced += produce(out + produced * channels_, outCap - produced);
    }
    return produced;
}

std::size_t Resampler::produce(float* out, std::size_t cap) noexcept {
    std::size_t n = 0;
    const std::size_t ch = static_cast<std::size_t>(channels_);
    while (n < cap) {
        const std::size_t i = static_cast<std::size_t>(pos_);
        if (i + static_cast<std::size_t>(half_) >= len_) break;
        const double f = pos_ - static_cast<double>(i);
        if (linear_) {
            const float a = static_cast<float>(f);
            const float* s = buf_.data() + i * ch;
            for (std::size_t c = 0; c < ch; ++c) out[n * ch + c] = s[c] * (1.0f - a) + s[ch + c] * a;
        } else {
            const double fp = f * phases_;
            const int p0 = static_cast<int>(fp);
            const float a = static_cast<float>(fp - p0);
            const float* r0 = table_.data() + static_cast<std::size_t>(p0) * taps_;
            const float* r1 = r0 + taps_;
            const float* s = buf_.data() + (i - static_cast<std::size_t>(half_ - 1)) * ch;
            for (std::size_t c = 0; c < ch; ++c) {
                float acc = 0.0f;
                for (int k = 0; k < taps_; ++k) {
                    const float w = r0[k] + a * (r1[k] - r0[k]);
                    acc += w * s[static_cast<std::size_t>(k) * ch + c];
                }
                out[n * ch + c] = acc;
            }
        }
        ++n;
        pos_ += step_;
    }
    // Compact: keep only the history still needed.
    const std::size_t i0 = static_cast<std::size_t>(pos_);
    const std::size_t drop = i0 >= static_cast<std::size_t>(half_ - 1) ? i0 - static_cast<std::size_t>(half_ - 1) : 0;
    if (drop > 0 && drop <= len_) {
        std::memmove(buf_.data(), buf_.data() + drop * ch, (len_ - drop) * ch * sizeof(float));
        len_ -= drop;
        pos_ -= static_cast<double>(drop);
    }
    return n;
}

}  // namespace hb::audio
