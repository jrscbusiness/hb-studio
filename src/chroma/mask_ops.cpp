#include "chroma/mask_ops.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace hb::chroma {

std::uint8_t probToByte(float p) {
    if (!(p > 0.f)) return 0;   // also catches NaN
    if (p >= 1.f) return 255;
    return static_cast<std::uint8_t>(p * 255.f + 0.5f);
}

void applyLevels(Mask& m, int lo, int hi) {
    if (!m.valid()) return;
    lo = std::clamp(lo, 0, 254);
    hi = std::clamp(hi, lo + 1, 255);
    std::uint8_t lut[256];
    for (int v = 0; v < 256; ++v) {
        if (v <= lo) { lut[v] = 0; continue; }
        if (v >= hi) { lut[v] = 255; continue; }
        const float t = static_cast<float>(v - lo) / static_cast<float>(hi - lo);
        lut[v] = static_cast<std::uint8_t>(t * t * (3.f - 2.f * t) * 255.f + 0.5f);
    }
    const std::size_t n = static_cast<std::size_t>(m.width) * m.height;
    for (std::size_t i = 0; i < n; ++i) m.data[i] = lut[m.data[i]];
}

namespace {
template <bool Max>
void morph3x3(Mask& m) {
    const int w = m.width, h = m.height;
    std::vector<std::uint8_t> tmp(m.data.size());
    // horizontal pass
    for (int y = 0; y < h; ++y) {
        const std::uint8_t* r = m.data.data() + static_cast<std::size_t>(y) * w;
        std::uint8_t* o = tmp.data() + static_cast<std::size_t>(y) * w;
        for (int x = 0; x < w; ++x) {
            std::uint8_t v = r[x];
            if (x > 0) v = Max ? std::max(v, r[x - 1]) : std::min(v, r[x - 1]);
            if (x + 1 < w) v = Max ? std::max(v, r[x + 1]) : std::min(v, r[x + 1]);
            o[x] = v;
        }
    }
    // vertical pass
    for (int y = 0; y < h; ++y) {
        std::uint8_t* o = m.data.data() + static_cast<std::size_t>(y) * w;
        for (int x = 0; x < w; ++x) {
            std::uint8_t v = tmp[static_cast<std::size_t>(y) * w + x];
            if (y > 0) { const auto u = tmp[static_cast<std::size_t>(y - 1) * w + x]; v = Max ? std::max(v, u) : std::min(v, u); }
            if (y + 1 < h) { const auto u = tmp[static_cast<std::size_t>(y + 1) * w + x]; v = Max ? std::max(v, u) : std::min(v, u); }
            o[x] = v;
        }
    }
}
}  // namespace

void erode(Mask& m, int passes) {
    if (!m.valid()) return;
    for (int i = 0; i < std::min(passes, 8); ++i) morph3x3<false>(m);
}
void dilate(Mask& m, int passes) {
    if (!m.valid()) return;
    for (int i = 0; i < std::min(passes, 8); ++i) morph3x3<true>(m);
}

void boxBlur(Mask& m, int radius) {
    if (!m.valid() || radius <= 0) return;
    radius = std::min(radius, 32);
    const int w = m.width, h = m.height;
    std::vector<std::uint8_t> tmp(m.data.size());
    const int win = 2 * radius + 1;
    // horizontal, running sum with clamped edges
    for (int y = 0; y < h; ++y) {
        const std::uint8_t* r = m.data.data() + static_cast<std::size_t>(y) * w;
        std::uint8_t* o = tmp.data() + static_cast<std::size_t>(y) * w;
        int sum = 0;
        for (int k = -radius; k <= radius; ++k) sum += r[std::clamp(k, 0, w - 1)];
        for (int x = 0; x < w; ++x) {
            o[x] = static_cast<std::uint8_t>((sum + win / 2) / win);
            sum += r[std::min(x + radius + 1, w - 1)] - r[std::max(x - radius, 0)];
        }
    }
    // vertical
    for (int x = 0; x < w; ++x) {
        int sum = 0;
        for (int k = -radius; k <= radius; ++k) sum += tmp[static_cast<std::size_t>(std::clamp(k, 0, h - 1)) * w + x];
        for (int y = 0; y < h; ++y) {
            m.data[static_cast<std::size_t>(y) * w + x] = static_cast<std::uint8_t>((sum + win / 2) / win);
            sum += tmp[static_cast<std::size_t>(std::min(y + radius + 1, h - 1)) * w + x] -
                   tmp[static_cast<std::size_t>(std::max(y - radius, 0)) * w + x];
        }
    }
}

void emaBlend(Mask& acc, const Mask& incoming, int wNew) {
    if (!incoming.valid()) return;
    if (!acc.valid() || acc.width != incoming.width || acc.height != incoming.height) {
        acc = incoming;
        return;
    }
    wNew = std::clamp(wNew, 1, 256);
    const int wOld = 256 - wNew;
    const std::size_t n = static_cast<std::size_t>(acc.width) * acc.height;
    for (std::size_t i = 0; i < n; ++i)
        acc.data[i] = static_cast<std::uint8_t>((acc.data[i] * wOld + incoming.data[i] * wNew + 128) >> 8);
}

void upscaleRow(const Mask& m, int w, int h, int y, std::uint8_t* row) {
    if (!m.valid() || w <= 0 || h <= 0) {
        if (row && w > 0) std::fill(row, row + w, std::uint8_t{0});
        return;
    }
    // 16.16 fixed point sample positions, pixel-centre aligned.
    const std::int64_t sy = ((static_cast<std::int64_t>(y) * 2 + 1) * m.height * 65536) / (static_cast<std::int64_t>(h) * 2) - 32768;
    std::int64_t y0 = sy >> 16;
    const int fy = static_cast<int>((sy & 0xFFFF) >> 8);   // 0..255
    std::int64_t y1 = y0 + 1;
    y0 = std::clamp<std::int64_t>(y0, 0, m.height - 1);
    y1 = std::clamp<std::int64_t>(y1, 0, m.height - 1);
    const std::uint8_t* r0 = m.data.data() + static_cast<std::size_t>(y0) * m.width;
    const std::uint8_t* r1 = m.data.data() + static_cast<std::size_t>(y1) * m.width;
    // Incremental 16.16 x position (no per-pixel division); the tiny rounding drift of `step` is
    // far below one mask pixel across a frame width.
    const std::int64_t step = (static_cast<std::int64_t>(m.width) * 65536) / w;
    std::int64_t sx = step / 2 - 32768;
    for (int x = 0; x < w; ++x, sx += step) {
        std::int64_t x0 = sx >> 16;
        const int fx = static_cast<int>((sx & 0xFFFF) >> 8);
        std::int64_t x1 = x0 + 1;
        x0 = std::clamp<std::int64_t>(x0, 0, m.width - 1);
        x1 = std::clamp<std::int64_t>(x1, 0, m.width - 1);
        const int top = r0[x0] * (256 - fx) + r0[x1] * fx;
        const int bot = r1[x0] * (256 - fx) + r1[x1] * fx;
        row[x] = static_cast<std::uint8_t>((top * (256 - fy) + bot * fy + 32768) >> 16);
    }
}

void upscaleBilinear(const Mask& m, Mask& out, int w, int h) {
    if (w <= 0 || h <= 0) { out.resize(0, 0); return; }
    out.resize(w, h);
    for (int y = 0; y < h; ++y) upscaleRow(m, w, h, y, out.data.data() + static_cast<std::size_t>(y) * w);
}

}  // namespace hb::chroma
