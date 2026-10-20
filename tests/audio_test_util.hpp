#pragma once
// Helpers shared by the audio tests (no audio device, deterministic signals).
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <vector>

#include "audio/audio_common.hpp"

namespace hb::audio::test {

// Interleaved stereo sine, same on both channels (or only left if rightOn false).
inline std::vector<float> stereoSine(double hz, double amp, double fs, std::size_t frames,
                                     double phase = 0.0, bool rightOn = true, bool leftOn = true) {
    std::vector<float> v(frames * 2);
    for (std::size_t i = 0; i < frames; ++i) {
        const float s = static_cast<float>(amp * std::sin(2.0 * kPi * hz * static_cast<double>(i) / fs + phase));
        v[2 * i] = leftOn ? s : 0.0f;
        v[2 * i + 1] = rightOn ? s : 0.0f;
    }
    return v;
}

inline std::vector<float> channel(const std::vector<float>& inter, int c, std::size_t skip = 0,
                                  std::size_t skipEnd = 0) {
    std::vector<float> o;
    const std::size_t n = inter.size() / 2;
    for (std::size_t i = skip; i + skipEnd < n; ++i) o.push_back(inter[2 * i + c]);
    return o;
}

// SNR in dB of x against the best-fit sine at `hz` (amplitude and phase free):
// everything that is not that sine (noise, distortion, images, aliases) is "noise".
inline double sineFitSnrDb(const std::vector<float>& x, double hz, double fs) {
    double ss = 0, cc = 0, sc = 0, ys = 0, yc = 0, yy = 0;
    for (std::size_t i = 0; i < x.size(); ++i) {
        const double w = 2.0 * kPi * hz * static_cast<double>(i) / fs;
        const double s = std::sin(w), c = std::cos(w), y = x[i];
        ss += s * s; cc += c * c; sc += s * c; ys += y * s; yc += y * c; yy += y * y;
    }
    const double det = ss * cc - sc * sc;
    const double a = (ys * cc - yc * sc) / det;
    const double b = (yc * ss - ys * sc) / det;
    const double fitE = a * a * ss + 2 * a * b * sc + b * b * cc;
    const double resid = std::max(1e-30, yy - fitE);
    return 10.0 * std::log10(fitE / resid);
}

// Plain radix-2 FFT (size must be a power of two).
inline void fft(std::vector<std::complex<double>>& a) {
    const std::size_t n = a.size();
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (std::size_t len = 2; len <= n; len <<= 1) {
        const std::complex<double> wl = std::polar(1.0, -2.0 * kPi / static_cast<double>(len));
        for (std::size_t i = 0; i < n; i += len) {
            std::complex<double> w = 1;
            for (std::size_t k = 0; k < len / 2; ++k) {
                const auto u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

inline double rmsDb(const std::vector<float>& x) {
    double s = 0;
    for (float v : x) s += static_cast<double>(v) * v;
    return 10.0 * std::log10(s / static_cast<double>(x.size()));
}

inline double maxAbsDiff(const std::vector<float>& x) {
    double m = 0;
    for (std::size_t i = 1; i < x.size(); ++i) m = std::max(m, std::fabs(static_cast<double>(x[i]) - x[i - 1]));
    return m;
}

}  // namespace hb::audio::test
