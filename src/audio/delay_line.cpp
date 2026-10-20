#include "audio/delay_line.hpp"

#include <algorithm>
#include <cmath>

namespace hb::audio {

DelayLine::DelayLine(int sampleRate, int channels, double maxDelayMs, double crossfadeMs)
    : sampleRate_(sampleRate),
      channels_(std::max(1, channels)),
      maxDelayMs_(maxDelayMs),
      maxDelaySamples_(static_cast<std::size_t>(std::llround(maxDelayMs * sampleRate / 1000.0))),
      capacity_(maxDelaySamples_ + 2),
      fadeLen_(std::max<std::size_t>(1, static_cast<std::size_t>(crossfadeMs * sampleRate / 1000.0))) {
    buf_.assign(capacity_ * static_cast<std::size_t>(channels_), 0.0f);
}

void DelayLine::reset() noexcept {
    std::fill(buf_.begin(), buf_.end(), 0.0f);
    w_ = 0;
    fading_ = false;
    fadePos_ = 0;
    curDelay_ = prevDelay_ = target_.load();
}

void DelayLine::setDelayMs(double ms) noexcept {
    if (!(ms >= 0.0)) ms = 0.0;  // also catches NaN
    ms = std::min(ms, maxDelayMs_);
    target_.store(static_cast<std::size_t>(std::llround(ms * sampleRate_ / 1000.0)),
                  std::memory_order_relaxed);
}

double DelayLine::delayMs() const noexcept {
    return 1000.0 * static_cast<double>(target_.load(std::memory_order_relaxed)) / sampleRate_;
}

void DelayLine::process(float* io, std::size_t frames) noexcept {
    const std::size_t ch = static_cast<std::size_t>(channels_);
    for (std::size_t i = 0; i < frames; ++i) {
        if (!fading_) {
            const std::size_t t = target_.load(std::memory_order_relaxed);
            if (t != curDelay_) {
                prevDelay_ = curDelay_;
                curDelay_ = t;
                fadePos_ = 0;
                fading_ = true;
            }
        }
        float* wr = &buf_[w_ * ch];
        for (std::size_t c = 0; c < ch; ++c) wr[c] = io[i * ch + c];
        const std::size_t rc = (w_ + capacity_ - curDelay_) % capacity_;
        const float* cur = &buf_[rc * ch];
        if (fading_) {
            const std::size_t rp = (w_ + capacity_ - prevDelay_) % capacity_;
            const float* prev = &buf_[rp * ch];
            const float a = static_cast<float>(fadePos_ + 1) / static_cast<float>(fadeLen_);
            for (std::size_t c = 0; c < ch; ++c) io[i * ch + c] = prev[c] * (1.0f - a) + cur[c] * a;
            if (++fadePos_ >= fadeLen_) fading_ = false;
        } else {
            for (std::size_t c = 0; c < ch; ++c) io[i * ch + c] = cur[c];
        }
        if (++w_ == capacity_) w_ = 0;
    }
}

}  // namespace hb::audio
