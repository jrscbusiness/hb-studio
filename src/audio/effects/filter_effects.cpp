// hb.highpass, hb.lowpass, hb.eq3  (RBJ cookbook biquads, double precision)
#include "audio/biquad.hpp"
#include "audio/effects/builtin_effects.hpp"
#include "audio/effects/effect_base.hpp"

namespace hb::audio::fx {
namespace {

void normalize(BiquadCoeffs& c, double a0) {
    c.b0 /= a0; c.b1 /= a0; c.b2 /= a0; c.a1 /= a0; c.a2 /= a0;
}

BiquadCoeffs passCoeffs(bool high, double f, double q, double sr) {
    const double w = 2.0 * kPi * f / sr, cs = std::cos(w), al = std::sin(w) / (2.0 * q);
    BiquadCoeffs c;
    if (high) { c.b0 = (1 + cs) / 2; c.b1 = -(1 + cs); c.b2 = (1 + cs) / 2; }
    else { c.b0 = (1 - cs) / 2; c.b1 = 1 - cs; c.b2 = (1 - cs) / 2; }
    c.a1 = -2 * cs; c.a2 = 1 - al;
    normalize(c, 1 + al);
    return c;
}

BiquadCoeffs shelfCoeffs(bool high, double f, double gainDb, double sr) {
    const double A = std::pow(10.0, gainDb / 40.0), w = 2.0 * kPi * f / sr, cs = std::cos(w);
    const double al = std::sin(w) / std::sqrt(2.0);  // slope S = 1
    const double t = 2.0 * std::sqrt(A) * al;
    BiquadCoeffs c;
    double a0;
    if (!high) {
        c.b0 = A * ((A + 1) - (A - 1) * cs + t);
        c.b1 = 2 * A * ((A - 1) - (A + 1) * cs);
        c.b2 = A * ((A + 1) - (A - 1) * cs - t);
        a0 = (A + 1) + (A - 1) * cs + t;
        c.a1 = -2 * ((A - 1) + (A + 1) * cs);
        c.a2 = (A + 1) + (A - 1) * cs - t;
    } else {
        c.b0 = A * ((A + 1) + (A - 1) * cs + t);
        c.b1 = -2 * A * ((A - 1) + (A + 1) * cs);
        c.b2 = A * ((A + 1) + (A - 1) * cs - t);
        a0 = (A + 1) - (A - 1) * cs + t;
        c.a1 = 2 * ((A - 1) - (A + 1) * cs);
        c.a2 = (A + 1) - (A - 1) * cs - t;
    }
    normalize(c, a0);
    return c;
}

BiquadCoeffs peakCoeffs(double f, double gainDb, double q, double sr) {
    const double A = std::pow(10.0, gainDb / 40.0), w = 2.0 * kPi * f / sr, cs = std::cos(w);
    const double al = std::sin(w) / (2.0 * q);
    BiquadCoeffs c;
    c.b0 = 1 + al * A; c.b1 = -2 * cs; c.b2 = 1 - al * A;
    c.a1 = -2 * cs; c.a2 = 1 - al / A;
    normalize(c, 1 + al / A);
    return c;
}

double clampFreq(double f, int sr) { return std::min(std::max(f, 5.0), 0.45 * sr); }

// ------------------------------------------------- hb.highpass / hb.lowpass
// 12 dB/oct: one Butterworth biquad (Q = 0.7071). 24 dB/oct: two cascaded biquads (4th-order Butterworth,
// Q = 0.5412 and 1.3066). Both topologies always run, so switching slope cross-fades (5 ms) without a click.
class PassFilterEffect final : public EffectBase {
public:
    explicit PassFilterEffect(bool high)
        : EffectBase(high ? "hb.highpass" : "hb.lowpass",
                     {high ? P("freq", "Frequência de corte", "Hz", 20.f, 500.f, 80.f, true)
                           : P("freq", "Frequência de corte", "Hz", 2000.f, 20000.f, 16000.f, true),
                      Choice("slope", "Inclinação", {"12 dB/oitava", "24 dB/oitava"}, 0)}),
          high_(high) {
        prepare(48000, 2);
    }

private:
    float targetLn() const noexcept { return std::log(p(0)); }
    float targetMix() const noexcept { return p(1) >= 0.5f ? 1.f : 0.f; }
    void onPrepare() override {
        mixA_ = static_cast<float>(1.0 - std::exp(-static_cast<double>(kSub) / (sr_ * 0.005)));
        onReset();
    }
    void onReset() override {
        for (auto& ch : st_) for (auto& s : ch) s.clear();
        lf_.snap(targetLn());
        mix_.snap(targetMix());
        updateCoeffs();
    }
    void updateCoeffs() {
        const double f = clampFreq(std::exp(static_cast<double>(lf_.v)), sr_);
        cA_ = passCoeffs(high_, f, 0.70710678118654752, sr_);
        cB1_ = passCoeffs(high_, f, 0.54119610014619698, sr_);
        cB2_ = passCoeffs(high_, f, 1.3065629648763766, sr_);
    }
    void processImpl(float* d, std::size_t frames) override {
        const std::size_t ch = static_cast<std::size_t>(ch_);
        for (std::size_t pos = 0; pos < frames; pos += kSub) {
            const std::size_t n = std::min(kSub, frames - pos);
            const float before = lf_.v;
            lf_.step(targetLn(), alpha_);
            if (lf_.v != before) updateCoeffs();
            const float m = mix_.step(targetMix(), mixA_);
            float* b = d + pos * ch;
            for (std::size_t i = 0; i < n; ++i) {
                for (std::size_t c = 0; c < ch; ++c) {
                    const double x = b[i * ch + c];
                    const double yA = st_[c][0].process(cA_, x);
                    const double y24 = st_[c][2].process(cB2_, st_[c][1].process(cB1_, x));
                    b[i * ch + c] = static_cast<float>(m <= 0.f ? yA : (m >= 1.f ? y24 : yA + m * (y24 - yA)));
                }
            }
            for (std::size_t c = 0; c < ch; ++c) for (auto& s : st_[c]) s.flush();
        }
    }
    bool high_;
    Smoothed lf_, mix_;
    float mixA_ = 0.1f;
    BiquadCoeffs cA_, cB1_, cB2_;
    BiquadState st_[kMaxChannels][3];
};

// ---------------------------------------------------------------- hb.eq3
class Eq3Effect final : public EffectBase {
public:
    Eq3Effect()
        : EffectBase("hb.eq3",
                     {P("low_freq", "Grave: frequência", "Hz", 40.f, 400.f, 100.f, true),
                      P("low_gain", "Grave: ganho", "dB", -15.f, 15.f, 0.f),
                      P("mid_freq", "Médio: frequência", "Hz", 200.f, 8000.f, 1000.f, true),
                      P("mid_gain", "Médio: ganho", "dB", -15.f, 15.f, 0.f),
                      P("mid_q", "Médio: Q", "", 0.3f, 10.f, 1.f, true),
                      P("high_freq", "Agudo: frequência", "Hz", 2000.f, 16000.f, 8000.f, true),
                      P("high_gain", "Agudo: ganho", "dB", -15.f, 15.f, 0.f)}) {
        prepare(48000, 2);
    }

private:
    void targets(float* t) const noexcept {
        t[0] = std::log(p(0)); t[1] = p(1); t[2] = std::log(p(2)); t[3] = p(3);
        t[4] = std::log(p(4)); t[5] = std::log(p(5)); t[6] = p(6);
    }
    void onPrepare() override { onReset(); }
    void onReset() override {
        for (auto& ch : st_) for (auto& s : ch) s.clear();
        float t[7];
        targets(t);
        for (int i = 0; i < 7; ++i) s_[i].snap(t[i]);
        updateCoeffs();
    }
    void updateCoeffs() {
        cLow_ = shelfCoeffs(false, clampFreq(std::exp(static_cast<double>(s_[0].v)), sr_), s_[1].v, sr_);
        cMid_ = peakCoeffs(clampFreq(std::exp(static_cast<double>(s_[2].v)), sr_), s_[3].v,
                           std::exp(static_cast<double>(s_[4].v)), sr_);
        cHigh_ = shelfCoeffs(true, clampFreq(std::exp(static_cast<double>(s_[5].v)), sr_), s_[6].v, sr_);
    }
    void processImpl(float* d, std::size_t frames) override {
        const std::size_t ch = static_cast<std::size_t>(ch_);
        float t[7];
        for (std::size_t pos = 0; pos < frames; pos += kSub) {
            const std::size_t n = std::min(kSub, frames - pos);
            targets(t);
            bool changed = false;
            for (int i = 0; i < 7; ++i) {
                const float b = s_[i].v;
                s_[i].step(t[i], alpha_);
                changed = changed || s_[i].v != b;
            }
            if (changed) updateCoeffs();
            float* b = d + pos * ch;
            for (std::size_t i = 0; i < n; ++i)
                for (std::size_t c = 0; c < ch; ++c) {
                    double x = b[i * ch + c];
                    x = st_[c][0].process(cLow_, x);
                    x = st_[c][1].process(cMid_, x);
                    x = st_[c][2].process(cHigh_, x);
                    b[i * ch + c] = static_cast<float>(x);
                }
            for (std::size_t c = 0; c < ch; ++c) for (auto& s : st_[c]) s.flush();
        }
    }
    Smoothed s_[7];
    BiquadCoeffs cLow_, cMid_, cHigh_;
    BiquadState st_[kMaxChannels][3];
};

}  // namespace

std::unique_ptr<Effect> makeHighpass() { return std::make_unique<PassFilterEffect>(true); }
std::unique_ptr<Effect> makeLowpass() { return std::make_unique<PassFilterEffect>(false); }
std::unique_ptr<Effect> makeEq3() { return std::make_unique<Eq3Effect>(); }

}  // namespace hb::audio::fx
