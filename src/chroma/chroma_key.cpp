#include "chroma/chroma_key.hpp"

#include <algorithm>
#include <cmath>

namespace hb::chroma {

void referenceKeyCbCr(Mode m, int& cb, int& cr) {
    if (m == Mode::Blue) { cb = 198; cr = 102; }   // ~ RGB (40,70,200)
    else { cb = 93; cr = 63; }                     // ~ RGB (40,190,70)
}

namespace {
inline bool frameOk(const video::Frame& f) {
    return f.width > 0 && f.height > 0 && f.stride >= f.width * 4 &&
           f.data.size() >= static_cast<std::size_t>(f.stride) * static_cast<std::size_t>(f.height);
}
}  // namespace

void computeKeyMask(const video::Frame& f, const KeyParams& p, Mask& out) {
    if (!frameOk(f)) { out.resize(0, 0); return; }
    out.resize(f.width, f.height);
    int kcb, kcr;
    referenceKeyCbCr(p.mode, kcb, kcr);
    if (p.keyCb >= 0) kcb = p.keyCb;
    if (p.keyCr >= 0) kcr = p.keyCr;
    const int tol = std::clamp(p.tolerance, 0, 100), soft = std::clamp(p.softness, 0, 100);
    // Squared-distance thresholds in fixed point avoid a sqrt per pixel: alpha is a LUT over the
    // integer distance (0..~360), built once per call.
    const float d0 = tol * 0.9f, band = soft * 0.9f + 1.f;
    std::uint8_t lut[512];
    for (int d = 0; d < 512; ++d) {
        const float t = (static_cast<float>(d) - d0) / band;
        if (t <= 0.f) lut[d] = 0;
        else if (t >= 1.f) lut[d] = 255;
        else lut[d] = static_cast<std::uint8_t>(t * t * (3.f - 2.f * t) * 255.f + 0.5f);
    }
    for (int y = 0; y < f.height; ++y) {
        const std::uint8_t* px = f.data.data() + static_cast<std::size_t>(y) * f.stride;
        std::uint8_t* o = out.data.data() + static_cast<std::size_t>(y) * f.width;
        for (int x = 0; x < f.width; ++x, px += 4) {
            const int b = px[0], g = px[1], r = px[2];
            // BT.601 full range, fixed point /256.
            const int cb = 128 + ((-43 * r - 85 * g + 128 * b) >> 8);
            const int cr = 128 + ((128 * r - 107 * g - 21 * b) >> 8);
            const int dcb = cb - kcb, dcr = cr - kcr;
            const int d = static_cast<int>(std::sqrt(static_cast<float>(dcb * dcb + dcr * dcr)));
            o[x] = lut[std::min(d, 511)];
        }
    }
}

void despill(video::Frame& f, Spill kind, int amount) {
    if (kind == Spill::None || amount <= 0 || !frameOk(f)) return;
    amount = std::min(amount, 100);
    for (int y = 0; y < f.height; ++y) {
        std::uint8_t* px = f.data.data() + static_cast<std::size_t>(y) * f.stride;
        for (int x = 0; x < f.width; ++x, px += 4) {
            const int b = px[0], g = px[1], r = px[2];
            if (kind == Spill::Green) {
                const int lim = std::max(r, b);
                if (g > lim) px[1] = static_cast<std::uint8_t>(g - (g - lim) * amount / 100);
            } else {
                const int lim = std::max(r, g);
                if (b > lim) px[0] = static_cast<std::uint8_t>(b - (b - lim) * amount / 100);
            }
        }
    }
}

}  // namespace hb::chroma
