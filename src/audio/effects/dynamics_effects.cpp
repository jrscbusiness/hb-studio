// hb.gate, hb.compressor, hb.limiter, hb.deesser
#include "audio/biquad.hpp"
#include "audio/effects/builtin_effects.hpp"
#include "audio/effects/effect_base.hpp"

namespace hb::audio::fx {
namespace {

// One-pole coefficient for a time constant in ms (63 % point): y += (1 - c) * (target - y).
double tcCoef(double ms, int sr) { return std::exp(-1.0 / (std::max(ms, 0.01) * 0.001 * sr)); }

float peakOf(const float* f, int ch) noexcept {
    float m = 0.f;
    for (int c = 0; c < ch; ++c) m = std::max(m, std::fabs(f[c]));
    return m;
}

// ---------------------------------------------------------------- hb.gate
// Peak detector (10 ms decay) linked across channels, 3 dB hysteresis, hold, exponential gain
// smoothing (attack when opening, release when closing). No look-ahead.
class GateEffect final : public EffectBase {
public:
    GateEffect()
        : EffectBase("hb.gate", {P("threshold_db", "Limiar", "dB", -80.f, 0.f, -50.f),
                                 P("range_db", "Redução (range)", "dB", 0.f, 80.f, 60.f),
                                 P("attack_ms", "Ataque", "ms", 0.1f, 100.f, 1.f, true),
                                 P("hold_ms", "Retenção (hold)", "ms", 0.f, 2000.f, 50.f),
                                 P("release_ms", "Liberação", "ms", 5.f, 2000.f, 150.f, true)}) {
        prepare(48000, 2);
    }

private:
    void onPrepare() override {
        detDecay_ = static_cast<float>(tcCoef(10.0, sr_));
        onReset();
    }
    void onReset() override {
        env_ = 0.f;
        open_ = false;
        hold_ = 0;
        g_ = static_cast<float>(fromDb(-p(1)));
    }
    void processImpl(float* d, std::size_t frames) override {
        const std::size_t ch = static_cast<std::size_t>(ch_);
        float worstGain = 1.f;
        for (std::size_t pos = 0; pos < frames; pos += kSub) {
            const std::size_t n = std::min(kSub, frames - pos);
            const float thr = static_cast<float>(fromDb(p(0)));
            const float closeThr = thr * 0.70794578f;  // -3 dB hysteresis
            const float floorG = static_cast<float>(fromDb(-p(1)));
            const float atk = static_cast<float>(tcCoef(p(2), sr_));
            const float rel = static_cast<float>(tcCoef(p(4), sr_));
            const int holdSamples = static_cast<int>(p(3) * 0.001f * static_cast<float>(sr_));
            float* b = d + pos * ch;
            for (std::size_t i = 0; i < n; ++i) {
                float* f = b + i * ch;
                const float pk = peakOf(f, ch_);
                env_ = std::max(pk, env_ * detDecay_);
                if (env_ > thr) { open_ = true; hold_ = holdSamples; }
                else if (open_ && env_ < closeThr) {
                    if (hold_ > 0) --hold_;
                    else open_ = false;
                }
                const float target = open_ ? 1.f : floorG;
                const float c = target > g_ ? atk : rel;
                g_ = target + (g_ - target) * c;
                worstGain = std::min(worstGain, g_);
                for (std::size_t k = 0; k < ch; ++k) f[k] *= g_;
            }
            if (env_ < 1e-20f) env_ = 0.f;
        }
        publishGr(static_cast<float>(-toDb(std::max(worstGain, 1e-9f))));
    }
    float env_ = 0.f, g_ = 1.f, detDecay_ = 0.f;
    bool open_ = false;
    int hold_ = 0;
};

// ---------------------------------------------------------------- hb.compressor
// Feed-forward, detector linked across channels, level in dB "peak-equivalent" (a sine of amplitude A
// reads 20*log10(A) in both modes), static curve with quadratic soft knee, gain reduction smoothed in dB
// (attack when GR increases, release when it decreases).
class CompressorEffect final : public EffectBase {
public:
    CompressorEffect()
        : EffectBase("hb.compressor",
                     {P("threshold_db", "Limiar", "dB", -60.f, 0.f, -18.f),
                      P("ratio", "Razão", ":1", 1.f, 20.f, 3.f, true),
                      P("attack_ms", "Ataque", "ms", 0.1f, 200.f, 10.f, true),
                      P("release_ms", "Liberação", "ms", 5.f, 2000.f, 120.f, true),
                      P("knee_db", "Joelho", "dB", 0.f, 24.f, 6.f),
                      P("makeup_db", "Ganho de compensação", "dB", 0.f, 24.f, 0.f),
                      Choice("detect", "Detecção", {"RMS", "Pico"}, 0)}) {
        prepare(48000, 2);
    }

private:
    void onPrepare() override {
        rmsC_ = tcCoef(10.0, sr_);
        onReset();
    }
    void onReset() override {
        ms_ = 0.0;
        grDb_ = 0.0;
        mk_.snap(p(5));
        mkLin_ = static_cast<float>(fromDb(mk_.v));
    }
    void processImpl(float* d, std::size_t frames) override {
        const std::size_t ch = static_cast<std::size_t>(ch_);
        double worst = 0.0;
        for (std::size_t pos = 0; pos < frames; pos += kSub) {
            const std::size_t n = std::min(kSub, frames - pos);
            const double thr = p(0), ratio = p(1), knee = p(4);
            const double atk = tcCoef(p(2), sr_), rel = tcCoef(p(3), sr_);
            const bool peak = p(6) >= 0.5f;
            const float newMk = static_cast<float>(fromDb(mk_.step(p(5), alpha_)));
            const float mkStep = (newMk - mkLin_) / static_cast<float>(n);
            float* b = d + pos * ch;
            for (std::size_t i = 0; i < n; ++i) {
                float* f = b + i * ch;
                double lvl;
                if (peak) {
                    lvl = toDb(std::max(static_cast<double>(peakOf(f, ch_)), 1e-9));
                } else {
                    double s = 0;
                    for (std::size_t k = 0; k < ch; ++k) s += static_cast<double>(f[k]) * f[k];
                    s /= static_cast<double>(ch);
                    ms_ = flushD(s + (ms_ - s) * rmsC_);
                    lvl = 10.0 * std::log10(2.0 * ms_ + 1e-18);
                }
                const double over = lvl - thr;
                double out;
                if (2.0 * over < -knee) out = lvl;
                else if (knee > 0.0 && 2.0 * std::fabs(over) <= knee) {
                    const double t = over + knee * 0.5;
                    out = lvl + (1.0 / ratio - 1.0) * t * t / (2.0 * knee);
                } else out = thr + over / ratio;
                const double tgt = std::max(lvl - out, 0.0);
                grDb_ = tgt + (grDb_ - tgt) * (tgt > grDb_ ? atk : rel);
                if (grDb_ < 1e-9) grDb_ = 0.0;
                worst = std::max(worst, grDb_);
                mkLin_ += mkStep;
                const float g = static_cast<float>(grDb_ > 0.0 ? std::exp(-grDb_ * 0.11512925464970229) : 1.0) * mkLin_;
                for (std::size_t k = 0; k < ch; ++k) f[k] *= g;
            }
            mkLin_ = newMk;
        }
        publishGr(static_cast<float>(worst));
    }
    double rmsC_ = 0.99, ms_ = 0.0, grDb_ = 0.0;
    Smoothed mk_;
    float mkLin_ = 1.f;
};

// ---------------------------------------------------------------- hb.limiter
// Look-ahead peak limiter (sample peak, not true peak). Per frame: r = ceiling/peak (<= 1); m = min of r over
// the last L frames; gain = average of the last L values of m (ramps the reduction in before the peak
// arrives); release is a one-pole recovery that can never exceed that gain. A final hard clamp to the
// ceiling is a safety net against rounding. Latency = L - 1 frames (limiterLatencyFrames()).
class LimiterEffect final : public EffectBase {
public:
    LimiterEffect()
        : EffectBase("hb.limiter", {P("ceiling_db", "Teto", "dBFS", -20.f, 0.f, -1.f),
                                    P("release_ms", "Liberação", "ms", 10.f, 1000.f, 100.f, true)}) {
        prepare(48000, 2);
    }

private:
    void onPrepare() override {
        L_ = static_cast<std::size_t>(limiterLatencyFrames(sr_)) + 1;
        audio_.assign(L_ * static_cast<std::size_t>(ch_), 0.f);
        r_.assign(L_, 1.f);
        m_.assign(L_, 1.f);
        onReset();
    }
    void onReset() override {
        std::fill(audio_.begin(), audio_.end(), 0.f);
        std::fill(r_.begin(), r_.end(), 1.f);
        std::fill(m_.begin(), m_.end(), 1.f);
        w_ = 0;
        sum_ = static_cast<double>(L_);
        rel_ = 1.f;
    }
    void processImpl(float* d, std::size_t frames) override {
        const std::size_t ch = static_cast<std::size_t>(ch_);
        float worst = 1.f;
        for (std::size_t pos = 0; pos < frames; pos += kSub) {
            const std::size_t n = std::min(kSub, frames - pos);
            const float ceil = static_cast<float>(fromDb(p(0)));
            const float relC = static_cast<float>(tcCoef(p(1), sr_));
            float* b = d + pos * ch;
            for (std::size_t i = 0; i < n; ++i) {
                float* f = b + i * ch;
                float* slot = &audio_[w_ * ch];
                for (std::size_t k = 0; k < ch; ++k) slot[k] = f[k];
                const float pk = peakOf(f, ch_);
                r_[w_] = pk > ceil ? ceil / pk : 1.f;
                float mn = 1.f;
                for (std::size_t k = 0; k < L_; ++k) mn = std::min(mn, r_[k]);
                sum_ += static_cast<double>(mn) - static_cast<double>(m_[w_]);
                m_[w_] = mn;
                const float avg = static_cast<float>(sum_ / static_cast<double>(L_));
                if (avg < rel_) rel_ = avg;
                else rel_ = avg - (avg - rel_) * relC;
                const std::size_t rd = (w_ + 1) % L_;
                const float* dl = &audio_[rd * ch];
                for (std::size_t k = 0; k < ch; ++k) f[k] = std::min(std::max(dl[k] * rel_, -ceil), ceil);
                worst = std::min(worst, rel_);
                if (++w_ == L_) {
                    w_ = 0;
                    double s = 0;  // resync the running sum (no drift)
                    for (std::size_t k = 0; k < L_; ++k) s += static_cast<double>(m_[k]);
                    sum_ = s;
                }
            }
        }
        publishGr(static_cast<float>(-toDb(std::max(worst, 1e-9f))));
    }
    std::size_t L_ = 1, w_ = 0;
    std::vector<float> audio_, r_, m_;
    double sum_ = 1.0;
    float rel_ = 1.f;
};

// ---------------------------------------------------------------- hb.deesser
// Split-band: a band-pass (unity gain at the centre, Q = 1.5) feeds a peak detector; the band component is
// attenuated and subtracted from the dry signal: y = x - (1 - g) * bp(x). Outside the band the signal is
// left (almost) untouched. Max reduction 24 dB.
class DeesserEffect final : public EffectBase {
public:
    DeesserEffect()
        : EffectBase("hb.deesser", {P("freq", "Frequência central", "Hz", 3000.f, 10000.f, 6500.f, true),
                                    P("threshold_db", "Limiar", "dB", -60.f, 0.f, -30.f),
                                    P("ratio", "Razão", ":1", 1.f, 20.f, 4.f, true)}) {
        prepare(48000, 2);
    }

private:
    static constexpr double kQ = 1.5;
    void updateCoeffs() {
        const double f = std::min(std::max(static_cast<double>(std::exp(lf_.v)), 100.0), 0.45 * sr_);
        const double w = 2.0 * kPi * f / sr_, cs = std::cos(w), al = std::sin(w) / (2.0 * kQ);
        bp_.b0 = al / (1 + al); bp_.b1 = 0; bp_.b2 = -al / (1 + al);
        bp_.a1 = -2 * cs / (1 + al); bp_.a2 = (1 - al) / (1 + al);
    }
    void onPrepare() override {
        envDecay_ = tcCoef(2.0, sr_);
        atk_ = tcCoef(0.5, sr_);
        rel_ = tcCoef(40.0, sr_);
        onReset();
    }
    void onReset() override {
        for (auto& s : st_) s.clear();
        lf_.snap(std::log(p(0)));
        updateCoeffs();
        env_ = 0.0;
        grDb_ = 0.0;
    }
    void processImpl(float* d, std::size_t frames) override {
        const std::size_t ch = static_cast<std::size_t>(ch_);
        double worst = 0.0;
        double band[kMaxChannels];
        for (std::size_t pos = 0; pos < frames; pos += kSub) {
            const std::size_t n = std::min(kSub, frames - pos);
            const float before = lf_.v;
            lf_.step(std::log(p(0)), alpha_);
            if (lf_.v != before) updateCoeffs();
            const double thr = p(1), ratio = p(2);
            float* b = d + pos * ch;
            for (std::size_t i = 0; i < n; ++i) {
                float* f = b + i * ch;
                double pk = 0;
                for (std::size_t k = 0; k < ch; ++k) {
                    band[k] = st_[k].process(bp_, static_cast<double>(f[k]));
                    pk = std::max(pk, std::fabs(band[k]));
                }
                env_ = flushD(std::max(pk, env_ * envDecay_));
                const double lvl = toDb(std::max(env_, 1e-9));
                const double tgt = std::min(std::max(lvl - thr, 0.0) * (1.0 - 1.0 / ratio), 24.0);
                grDb_ = tgt + (grDb_ - tgt) * (tgt > grDb_ ? atk_ : rel_);
                if (grDb_ < 1e-9) grDb_ = 0.0;
                worst = std::max(worst, grDb_);
                const double cut = 1.0 - std::exp(-grDb_ * 0.11512925464970229);  // 1 - g
                for (std::size_t k = 0; k < ch; ++k) f[k] = static_cast<float>(f[k] - cut * band[k]);
            }
            for (std::size_t k = 0; k < ch; ++k) st_[k].flush();
        }
        publishGr(static_cast<float>(worst));
    }
    Smoothed lf_;
    BiquadCoeffs bp_;
    BiquadState st_[kMaxChannels];
    double env_ = 0.0, grDb_ = 0.0, envDecay_ = 0.9, atk_ = 0.9, rel_ = 0.99;
};

}  // namespace

int limiterLatencyFrames(int sampleRate) noexcept {
    const int sr = std::min(std::max(sampleRate, 8000), 384000);
    return std::max(1, static_cast<int>(std::lround(sr * 0.0015)));  // 1.5 ms
}

std::unique_ptr<Effect> makeGate() { return std::make_unique<GateEffect>(); }
std::unique_ptr<Effect> makeCompressor() { return std::make_unique<CompressorEffect>(); }
std::unique_ptr<Effect> makeLimiter() { return std::make_unique<LimiterEffect>(); }
std::unique_ptr<Effect> makeDeesser() { return std::make_unique<DeesserEffect>(); }

}  // namespace hb::audio::fx
