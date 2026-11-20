#pragma once
// Shared plumbing for the built-in effects: atomic parameter store, control-rate smoothing,
// input/output sanitising (NaN/Inf/denormals). Not part of the frozen contract.
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "audio/effect.hpp"

namespace hb::audio::fx {

inline constexpr int kMaxChannels = 8;
inline constexpr std::size_t kSub = 32;       // control-rate sub-block (frames)
inline constexpr double kParamTauSec = 0.02;  // parameter smoothing time constant
inline constexpr int kMaxParams = 16;

inline double fromDb(double db) noexcept { return std::pow(10.0, db / 20.0); }
inline double toDb(double lin) noexcept { return 20.0 * std::log10(lin); }
inline double flushD(double x) noexcept { return std::fabs(x) < 1e-30 ? 0.0 : x; }

// One-pole parameter smoother stepped once per sub-block.
struct Smoothed {
    float v = 0.f;
    void snap(float t) noexcept { v = t; }
    float step(float target, float alpha) noexcept {
        const float d = target - v;
        if (std::fabs(d) <= 1e-6f * std::max(1.f, std::fabs(target))) v = target;
        else v += alpha * d;
        return v;
    }
};

// Transposed direct form II state (double precision, denormal-flushed by flush()).
struct BiquadState {
    double z1 = 0, z2 = 0;
    template <class C>
    double process(const C& c, double x) noexcept {
        const double y = c.b0 * x + z1;
        z1 = c.b1 * x - c.a1 * y + z2;
        z2 = c.b2 * x - c.a2 * y;
        return y;
    }
    void flush() noexcept { z1 = flushD(z1); z2 = flushD(z2); }
    void clear() noexcept { z1 = z2 = 0; }
};

// Linear gain ramp g0 -> g1 over n frames (interleaved, ch channels).
inline void applyRamp(float* b, std::size_t n, int ch, float g0, float g1) noexcept {
    if (g0 == g1) {
        if (g0 == 1.f) return;
        for (std::size_t i = 0; i < n * static_cast<std::size_t>(ch); ++i) b[i] *= g0;
        return;
    }
    const float step = (g1 - g0) / static_cast<float>(n);
    float g = g0;
    for (std::size_t i = 0; i < n; ++i) {
        g += step;
        for (int c = 0; c < ch; ++c) b[i * static_cast<std::size_t>(ch) + static_cast<std::size_t>(c)] *= g;
    }
}

class EffectBase : public Effect {
public:
    const char* typeId() const override { return typeId_; }
    const std::vector<EffectParamInfo>& params() const override { return params_; }

    void setParam(const std::string& id, float value) override {
        const int i = indexOf(id);
        if (i < 0 || !std::isfinite(value)) return;
        const auto& p = params_[static_cast<std::size_t>(i)];
        float v = std::min(std::max(value, p.min), p.max);
        if (!p.choices.empty()) v = std::floor(v + 0.5f);
        values_[static_cast<std::size_t>(i)].store(v, std::memory_order_relaxed);
    }
    float getParam(const std::string& id) const override {
        const int i = indexOf(id);
        return i < 0 ? 0.f : values_[static_cast<std::size_t>(i)].load(std::memory_order_relaxed);
    }
    void prepare(int sampleRate, int channels) override {
        sr_ = std::min(std::max(sampleRate, 8000), 384000);
        ch_ = std::min(std::max(channels, 1), kMaxChannels);
        alpha_ = static_cast<float>(1.0 - std::exp(-static_cast<double>(kSub) / (sr_ * kParamTauSec)));
        gr_.store(0.f, std::memory_order_relaxed);
        onPrepare();
        prepared_ = true;
    }
    void process(float* d, std::size_t frames) override {
        if (!prepared_ || d == nullptr || frames == 0) return;
        const std::size_t n = frames * static_cast<std::size_t>(ch_);
        for (std::size_t i = 0; i < n; ++i)
            if (!std::isfinite(d[i])) d[i] = 0.f;
        processImpl(d, frames);
        bool bad = false;
        for (std::size_t i = 0; i < n; ++i) {
            const float x = d[i];
            if (!std::isfinite(x)) { d[i] = 0.f; bad = true; }
            else if (std::fabs(x) < 1e-30f) d[i] = 0.f;
        }
        if (bad) onReset();
    }
    void reset() override { if (prepared_) onReset(); }
    float gainReductionDb() const override { return gr_.load(std::memory_order_relaxed); }

protected:
    EffectBase(const char* typeId, std::vector<EffectParamInfo> ps) : typeId_(typeId), params_(std::move(ps)) {
        for (std::size_t i = 0; i < params_.size() && i < values_.size(); ++i)
            values_[i].store(params_[i].def, std::memory_order_relaxed);
    }
    virtual void onPrepare() = 0;   // (re)allocate and clear state / snap smoothers
    virtual void onReset() = 0;     // clear state, snap smoothers to current targets
    virtual void processImpl(float* interleaved, std::size_t frames) = 0;

    float p(int i) const noexcept { return values_[static_cast<std::size_t>(i)].load(std::memory_order_relaxed); }
    void publishGr(float db) noexcept { gr_.store(db > 0.f && std::isfinite(db) ? db : 0.f, std::memory_order_relaxed); }

    int sr_ = 48000;
    int ch_ = 2;
    float alpha_ = 0.f;  // smoother coefficient per sub-block

    static EffectParamInfo P(const char* id, const char* label, const char* unit, float mn, float mx, float df,
                             bool log = false) {
        EffectParamInfo e;
        e.id = id; e.label = label; e.unit = unit; e.min = mn; e.max = mx; e.def = df; e.logarithmic = log;
        return e;
    }
    static EffectParamInfo Choice(const char* id, const char* label, std::vector<std::string> ch, float df = 0) {
        EffectParamInfo e;
        e.id = id; e.label = label; e.min = 0; e.max = static_cast<float>(ch.size() - 1); e.def = df;
        e.choices = std::move(ch);
        return e;
    }

private:
    int indexOf(const std::string& id) const noexcept {
        for (std::size_t i = 0; i < params_.size(); ++i)
            if (params_[i].id == id) return static_cast<int>(i);
        return -1;
    }
    const char* typeId_;
    std::vector<EffectParamInfo> params_;
    std::array<std::atomic<float>, kMaxParams> values_{};
    std::atomic<float> gr_{0.f};
    bool prepared_ = false;
};

}  // namespace hb::audio::fx
