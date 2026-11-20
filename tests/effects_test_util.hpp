#pragma once
// Helpers shared by the effects tests.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <utility>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "audio/effect.hpp"

namespace hb::test {

inline constexpr double kPi = 3.14159265358979323846;

inline std::unique_ptr<hb::audio::Effect> make(const std::string& id, int sr = 48000, int ch = 2) {
    hb::audio::registerBuiltinEffects();
    auto e = hb::audio::EffectRegistry::instance().create(id);
    if (e) e->prepare(sr, ch);
    return e;
}

// Processes `buf` (interleaved) in blocks of `block` frames.
inline void run(hb::audio::Effect& e, std::vector<float>& buf, int ch, std::size_t block = 480) {
    const std::size_t frames = buf.size() / static_cast<std::size_t>(ch);
    for (std::size_t pos = 0; pos < frames; pos += block) {
        const std::size_t n = std::min(block, frames - pos);
        e.process(buf.data() + pos * static_cast<std::size_t>(ch), n);
    }
}

inline std::vector<float> sine(double hz, double ampLin, int sr, std::size_t frames, int ch, double phase = 0.0) {
    std::vector<float> v(frames * static_cast<std::size_t>(ch));
    for (std::size_t i = 0; i < frames; ++i) {
        const float s = static_cast<float>(ampLin * std::sin(2.0 * kPi * hz * static_cast<double>(i) / sr + phase));
        for (int c = 0; c < ch; ++c) v[i * static_cast<std::size_t>(ch) + static_cast<std::size_t>(c)] = s;
    }
    return v;
}

// RMS of channel `c` over frames [from, to).
inline double rms(const std::vector<float>& v, int ch, std::size_t from, std::size_t to, int c = 0) {
    double s = 0;
    for (std::size_t i = from; i < to; ++i) {
        const double x = v[i * static_cast<std::size_t>(ch) + static_cast<std::size_t>(c)];
        s += x * x;
    }
    return std::sqrt(s / static_cast<double>(to - from));
}

inline double db(double lin) { return 20.0 * std::log10(std::max(lin, 1e-12)); }

// Steady-state gain in dB of an effect at `hz` (sine of amplitude `amp`), measured after `settle` frames.
inline double gainDbAt(hb::audio::Effect& e, double hz, int sr, double amp = 0.1, int ch = 2) {
    e.reset();
    const std::size_t frames = static_cast<std::size_t>(sr);   // 1 s
    auto v = sine(hz, amp, sr, frames, ch);
    run(e, v, ch);
    const std::size_t a = frames / 2;
    return db(rms(v, ch, a, frames) / (amp / std::sqrt(2.0)));
}

inline std::vector<float> noise(std::size_t frames, int ch, float amp, unsigned seed = 1) {
    std::mt19937 g(seed);
    std::uniform_real_distribution<float> d(-amp, amp);
    std::vector<float> v(frames * static_cast<std::size_t>(ch));
    for (auto& x : v) x = d(g);
    return v;
}

inline void setAll(hb::audio::Effect& e, std::initializer_list<std::pair<const char*, float>> kv) {
    for (auto& p : kv) e.setParam(p.first, p.second);
}

}  // namespace hb::test
