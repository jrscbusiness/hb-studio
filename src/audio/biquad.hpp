#pragma once
#include <cmath>
#include <complex>

#include "audio/audio_common.hpp"

namespace hb::audio {

// Normalised biquad (a0 == 1): y = b0 x + b1 x1 + b2 x2 - a1 y1 - a2 y2.
struct BiquadCoeffs {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
};

// Transposed direct form II, double precision.
class Biquad {
public:
    Biquad() = default;
    explicit Biquad(const BiquadCoeffs& c) : c_(c) {}
    void setCoeffs(const BiquadCoeffs& c) { c_ = c; }
    void reset() { z1_ = z2_ = 0; }
    double process(double x) noexcept {
        const double y = c_.b0 * x + z1_;
        z1_ = c_.b1 * x - c_.a1 * y + z2_;
        z2_ = c_.b2 * x - c_.a2 * y;
        return y;
    }

private:
    BiquadCoeffs c_{};
    double z1_ = 0, z2_ = 0;
};

// |H(e^jw)| at `hz` for sample rate `sr` (used for analytic validation).
inline double biquadMagnitude(const BiquadCoeffs& c, double hz, double sr) {
    const double w = 2.0 * kPi * hz / sr;
    const std::complex<double> z1 = std::polar(1.0, -w), z2 = std::polar(1.0, -2.0 * w);
    const std::complex<double> num = c.b0 + c.b1 * z1 + c.b2 * z2;
    const std::complex<double> den = 1.0 + c.a1 * z1 + c.a2 * z2;
    return std::abs(num / den);
}

}  // namespace hb::audio
