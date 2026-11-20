// hb.gain, hb.phase, hb.mono, hb.delay
#include <cstdint>

#include "audio/effects/builtin_effects.hpp"
#include "audio/effects/effect_base.hpp"

namespace hb::audio::fx {
namespace {

// ---------------------------------------------------------------- hb.gain
class GainEffect final : public EffectBase {
public:
    GainEffect() : EffectBase("hb.gain", {P("gain_db", "Ganho", "dB", -24.f, 24.f, 0.f)}) { prepare(48000, 2); }

private:
    void onPrepare() override { onReset(); }
    void onReset() override {
        db_.snap(p(0));
        lin_ = static_cast<float>(fromDb(db_.v));
    }
    void processImpl(float* d, std::size_t frames) override {
        for (std::size_t pos = 0; pos < frames; pos += kSub) {
            const std::size_t n = std::min(kSub, frames - pos);
            const float nl = static_cast<float>(fromDb(db_.step(p(0), alpha_)));
            applyRamp(d + pos * static_cast<std::size_t>(ch_), n, ch_, lin_, nl);
            lin_ = nl;
        }
    }
    Smoothed db_;
    float lin_ = 1.f;
};

// ---------------------------------------------------------------- hb.phase
class PhaseEffect final : public EffectBase {
public:
    PhaseEffect() : EffectBase("hb.phase", {Choice("invert", "Polaridade", {"Normal", "Invertida"})}) { prepare(48000, 2); }

private:
    float target() const noexcept { return p(0) >= 0.5f ? -1.f : 1.f; }
    void onPrepare() override {
        a_ = static_cast<float>(1.0 - std::exp(-static_cast<double>(kSub) / (sr_ * 0.005)));
        onReset();
    }
    void onReset() override { s_.snap(target()); cur_ = s_.v; }
    void processImpl(float* d, std::size_t frames) override {
        for (std::size_t pos = 0; pos < frames; pos += kSub) {
            const std::size_t n = std::min(kSub, frames - pos);
            const float ng = s_.step(target(), a_);
            applyRamp(d + pos * static_cast<std::size_t>(ch_), n, ch_, cur_, ng);
            cur_ = ng;
        }
    }
    Smoothed s_;
    float cur_ = 1.f;
    float a_ = 0.1f;
};

// ---------------------------------------------------------------- hb.mono
class MonoEffect final : public EffectBase {
public:
    MonoEffect() : EffectBase("hb.mono", {Choice("mode", "Modo", {"L+R", "Somente L", "Somente R"})}) { prepare(48000, 2); }

private:
    float target() const noexcept {
        const int m = static_cast<int>(p(0));
        return m == 1 ? 1.f : (m == 2 ? 0.f : 0.5f);   // weight of L; R gets 1 - a
    }
    void onPrepare() override {
        a_ = static_cast<float>(1.0 - std::exp(-static_cast<double>(kSub) / (sr_ * 0.005)));
        onReset();
    }
    void onReset() override { s_.snap(target()); cur_ = s_.v; }
    void processImpl(float* d, std::size_t frames) override {
        if (ch_ < 2) return;
        const std::size_t ch = static_cast<std::size_t>(ch_);
        for (std::size_t pos = 0; pos < frames; pos += kSub) {
            const std::size_t n = std::min(kSub, frames - pos);
            const float na = s_.step(target(), a_);
            const float step = (na - cur_) / static_cast<float>(n);
            float a = cur_;
            float* b = d + pos * ch;
            for (std::size_t i = 0; i < n; ++i) {
                a += step;
                const float m = a * b[i * ch] + (1.f - a) * b[i * ch + 1];
                b[i * ch] = m;
                b[i * ch + 1] = m;
            }
            cur_ = na;
        }
    }
    Smoothed s_;
    float cur_ = 0.5f;
    float a_ = 0.1f;
};

// ---------------------------------------------------------------- hb.delay
// Integer-sample delay; when the target changes, a 10 ms cross-fade between the old and the new tap
// avoids clicks (no pitch glide).
class DelayEffect final : public EffectBase {
public:
    DelayEffect() : EffectBase("hb.delay", {P("time_ms", "Atraso", "ms", 0.f, 2000.f, 0.f)}) { prepare(48000, 2); }

private:
    int targetSamples() const noexcept {
        const double s = std::floor(static_cast<double>(p(0)) * sr_ / 1000.0 + 0.5);
        return static_cast<int>(std::min(std::max(s, 0.0), static_cast<double>(maxDelay_)));
    }
    void onPrepare() override {
        maxDelay_ = static_cast<int>(2000LL * sr_ / 1000);
        std::size_t frames = 1;
        while (frames < static_cast<std::size_t>(maxDelay_) + 2) frames <<= 1;
        buf_.assign(frames * static_cast<std::size_t>(ch_), 0.f);
        mask_ = frames - 1;
        fadeLen_ = std::max(1, sr_ / 100);
        onReset();
    }
    void onReset() override {
        std::fill(buf_.begin(), buf_.end(), 0.f);
        write_ = 0;
        cur_ = old_ = targetSamples();
        fading_ = false;
        fadePos_ = 0;
    }
    void processImpl(float* d, std::size_t frames) override {
        const std::size_t ch = static_cast<std::size_t>(ch_);
        for (std::size_t pos = 0; pos < frames; pos += kSub) {
            const std::size_t n = std::min(kSub, frames - pos);
            if (!fading_) {
                const int t = targetSamples();
                if (t != cur_) { old_ = cur_; cur_ = t; fading_ = true; fadePos_ = 0; }
            }
            for (std::size_t i = 0; i < n; ++i) {
                float* f = d + (pos + i) * ch;
                float* w = &buf_[write_ * ch];
                for (std::size_t c = 0; c < ch; ++c) w[c] = f[c];
                const float* a = &buf_[((write_ - static_cast<std::size_t>(cur_)) & mask_) * ch];
                if (fading_) {
                    const float* b = &buf_[((write_ - static_cast<std::size_t>(old_)) & mask_) * ch];
                    const float t = static_cast<float>(fadePos_) / static_cast<float>(fadeLen_);
                    for (std::size_t c = 0; c < ch; ++c) f[c] = (1.f - t) * b[c] + t * a[c];
                    if (++fadePos_ >= fadeLen_) fading_ = false;
                } else {
                    for (std::size_t c = 0; c < ch; ++c) f[c] = a[c];
                }
                write_ = (write_ + 1) & mask_;
            }
        }
    }
    std::vector<float> buf_;
    std::size_t mask_ = 0, write_ = 0;
    int maxDelay_ = 0, cur_ = 0, old_ = 0, fadePos_ = 0, fadeLen_ = 480;
    bool fading_ = false;
};

}  // namespace

std::unique_ptr<Effect> makeGain() { return std::make_unique<GainEffect>(); }
std::unique_ptr<Effect> makePhase() { return std::make_unique<PhaseEffect>(); }
std::unique_ptr<Effect> makeMono() { return std::make_unique<MonoEffect>(); }
std::unique_ptr<Effect> makeDelay() { return std::make_unique<DelayEffect>(); }

}  // namespace hb::audio::fx
