#include "audio/limiter.hpp"

#include <algorithm>
#include <cmath>

namespace hb::audio {

Limiter::Limiter(int sampleRate, LimiterConfig cfg)
    : sr_(sampleRate),
      knee_(cfg.softKneeDb),
      mode_(static_cast<int>(cfg.mode)),
      ceilingDb_(cfg.ceilingDb),
      ceilingLin_(dbToLinear(cfg.ceilingDb)),
      n_(std::max<std::size_t>(1, static_cast<std::size_t>(std::lround(cfg.lookaheadMs * sampleRate / 1000.0)))),
      delay_(n_ - 1),
      relCoef_(1.0f - std::exp(-1.0f / (std::max(1.0f, cfg.releaseMs) * 0.001f * static_cast<float>(sampleRate)))) {
    audio_.assign(n_ * 2, 0.0f);
    req_.assign(n_, 1.0f);
    avg_.assign(n_, 1.0f);
    reset();
}

void Limiter::setCeilingDb(float db) noexcept {
    if (!(db <= 0.0f)) db = 0.0f;
    db = std::max(db, -60.0f);
    ceilingDb_.store(db, std::memory_order_relaxed);
    ceilingLin_.store(dbToLinear(db), std::memory_order_relaxed);
}

void Limiter::reset() noexcept {
    std::fill(audio_.begin(), audio_.end(), 0.0f);
    std::fill(req_.begin(), req_.end(), 1.0f);
    std::fill(avg_.begin(), avg_.end(), 1.0f);
    w_ = 0;
    sum_ = static_cast<double>(n_);
    rel_ = 1.0f;
    lastGrDb_ = 0.0f;
}

void Limiter::process(float* io, std::size_t frames) noexcept {
    const LimiterMode mode = this->mode();
    const float ceil = ceilingLin_.load(std::memory_order_relaxed);
    const float kneeLin = ceil * dbToLinear(knee_);
    const float span = ceil - kneeLin;
    float minGain = 1.0f;

    for (std::size_t i = 0; i < frames; ++i) {
        const float xl = io[2 * i], xr = io[2 * i + 1];

        // Required gain for this input sample, with one-pole release.
        const float pk = std::max(std::fabs(xl), std::fabs(xr));
        const float want = pk > ceil ? ceil / pk : 1.0f;
        const float relaxed = rel_ + (1.0f - rel_) * relCoef_;
        rel_ = std::min(want, relaxed);
        req_[w_] = rel_;

        // Min over the last n_ required gains, then running mean of that.
        float m = req_[0];
        for (std::size_t k = 1; k < n_; ++k) m = std::min(m, req_[k]);
        sum_ += static_cast<double>(m) - static_cast<double>(avg_[w_]);
        avg_[w_] = m;
        const float gain = static_cast<float>(sum_ / static_cast<double>(n_));

        // Delayed audio (delay_ samples).
        const std::size_t rd = (w_ + n_ - delay_) % n_;
        float yl, yr;
        if (delay_ == 0) { yl = xl; yr = xr; }
        else { yl = audio_[2 * rd]; yr = audio_[2 * rd + 1]; }
        audio_[2 * w_] = xl;
        audio_[2 * w_ + 1] = xr;

        switch (mode) {
            case LimiterMode::Limiter:
                yl *= gain; yr *= gain;
                minGain = std::min(minGain, gain);
                break;
            case LimiterMode::SoftClip: {
                auto sc = [&](float v) {
                    const float a = std::fabs(v);
                    if (a <= kneeLin) return v;
                    const float o = kneeLin + span * std::tanh((a - kneeLin) / span);
                    return v < 0 ? -o : o;
                };
                yl = sc(yl); yr = sc(yr);
                break;
            }
            case LimiterMode::Off:
                break;
        }
        io[2 * i] = yl;
        io[2 * i + 1] = yr;

        if (++w_ == n_) {
            w_ = 0;
            double s = 0;  // resync the running sum once per window
            for (float v : avg_) s += v;
            sum_ = s;
        }
    }
    lastGrDb_ = linearToDb(minGain);
    if (lastGrDb_ > 0.0f) lastGrDb_ = 0.0f;
}

}  // namespace hb::audio
