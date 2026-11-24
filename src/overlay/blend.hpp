#pragma once
// Alpha convention (the contract every overlay layer follows):
//   * Program frames (video::Frame, BGRA8) are OPAQUE: the alpha byte is 255 and is left untouched
//     when something is drawn over them.
//   * Layer bitmaps (overlay::Bitmap, BGRA8) use STRAIGHT (non-premultiplied) alpha: the colour bytes
//     are the real colour, the alpha byte is the coverage. A fully transparent pixel is (0,0,0,0).
//   * "over" blending, per channel, with a = srcAlpha * opacity / 255:
//         out = (src * a + dst * (255 - a) + 127) / 255
//     When the destination is itself translucent (building a card from several pieces) the general
//     straight-alpha "over" operator is used so the result is still straight alpha.
#include <algorithm>
#include <cstdint>
#include <vector>

#include "video/program_output.hpp"

namespace hb::overlay {

struct Bitmap {
    int w = 0, h = 0;
    std::vector<std::uint8_t> bgra;  // w * h * 4, straight alpha, row stride = w * 4
    bool empty() const { return w <= 0 || h <= 0 || bgra.size() < static_cast<std::size_t>(w) * h * 4; }
    void resize(int nw, int nh) {
        w = std::max(0, nw);
        h = std::max(0, nh);
        bgra.assign(static_cast<std::size_t>(w) * h * 4, 0);
    }
};

struct Rect {
    int x = 0, y = 0, w = 0, h = 0;
    bool empty() const { return w <= 0 || h <= 0; }
};

// 0xRRGGBB packed colour helpers.
inline std::uint8_t colR(std::uint32_t c) { return static_cast<std::uint8_t>((c >> 16) & 0xFF); }
inline std::uint8_t colG(std::uint32_t c) { return static_cast<std::uint8_t>((c >> 8) & 0xFF); }
inline std::uint8_t colB(std::uint32_t c) { return static_cast<std::uint8_t>(c & 0xFF); }

// Blends one straight-alpha source pixel (b,g,r,a) over `d` (4 bytes BGRA).
inline void blendPixel(std::uint8_t* d, std::uint8_t b, std::uint8_t g, std::uint8_t r, int a) {
    if (a <= 0) return;
    if (a > 255) a = 255;
    const int da = d[3];
    if (da == 255) {
        const int ia = 255 - a;
        d[0] = static_cast<std::uint8_t>((b * a + d[0] * ia + 127) / 255);
        d[1] = static_cast<std::uint8_t>((g * a + d[1] * ia + 127) / 255);
        d[2] = static_cast<std::uint8_t>((r * a + d[2] * ia + 127) / 255);
        return;
    }
    // General straight-alpha over: outA = a + da * (1 - a).
    const int outA = a * 255 + da * (255 - a);  // scaled by 255
    if (outA <= 0) return;
    const int wd = da * (255 - a);              // weight of the destination
    const int ws = a * 255;                     // weight of the source
    d[0] = static_cast<std::uint8_t>((b * ws + d[0] * wd + outA / 2) / outA);
    d[1] = static_cast<std::uint8_t>((g * ws + d[1] * wd + outA / 2) / outA);
    d[2] = static_cast<std::uint8_t>((r * ws + d[2] * wd + outA / 2) / outA);
    d[3] = static_cast<std::uint8_t>((outA + 127) / 255);
}

// Clips a w*h rectangle placed at (x,y) against a (cw,ch) canvas. Returns false if nothing is visible.
inline bool clipRect(int x, int y, int w, int h, int cw, int ch, Rect& out) {
    const int x0 = std::max(0, x), y0 = std::max(0, y);
    const int x1 = std::min(cw, x + w), y1 = std::min(ch, y + h);
    if (x1 <= x0 || y1 <= y0) return false;
    out = {x0, y0, x1 - x0, y1 - y0};
    return true;
}

inline bool frameValid(const video::Frame& f) {
    return f.width > 0 && f.height > 0 && f.stride >= f.width * 4 &&
           f.data.size() >= static_cast<std::size_t>(f.stride) * static_cast<std::size_t>(f.height);
}

// Draws `src` over `dst` at (dx,dy) with an extra opacity in [0,1]. Only the dirty rectangle (the part
// of the bitmap inside the canvas) is touched. Never reads or writes out of bounds.
inline void blendBitmap(video::Frame& dst, const Bitmap& src, int dx, int dy, double opacity = 1.0) {
    if (src.empty() || !frameValid(dst)) return;
    const int op = static_cast<int>(std::clamp(opacity, 0.0, 1.0) * 256.0 + 0.5);  // 0..256
    if (op == 0) return;
    Rect r;
    if (!clipRect(dx, dy, src.w, src.h, dst.width, dst.height, r)) return;
    for (int y = 0; y < r.h; ++y) {
        const std::uint8_t* s = src.bgra.data() + (static_cast<std::size_t>(y + r.y - dy) * src.w + (r.x - dx)) * 4;
        std::uint8_t* d = dst.data.data() + static_cast<std::size_t>(y + r.y) * dst.stride + static_cast<std::size_t>(r.x) * 4;
        for (int x = 0; x < r.w; ++x, s += 4, d += 4) {
            if (s[3] == 0) continue;
            blendPixel(d, s[0], s[1], s[2], op >= 256 ? s[3] : (s[3] * op) >> 8);
        }
    }
}

// Same, but into another (straight-alpha) bitmap: used to compose a card out of pieces.
inline void blendBitmap(Bitmap& dst, const Bitmap& src, int dx, int dy) {
    if (src.empty() || dst.empty()) return;
    Rect r;
    if (!clipRect(dx, dy, src.w, src.h, dst.w, dst.h, r)) return;
    for (int y = 0; y < r.h; ++y) {
        const std::uint8_t* s = src.bgra.data() + (static_cast<std::size_t>(y + r.y - dy) * src.w + (r.x - dx)) * 4;
        std::uint8_t* d = dst.bgra.data() + (static_cast<std::size_t>(y + r.y) * dst.w + r.x) * 4;
        for (int x = 0; x < r.w; ++x, s += 4, d += 4)
            if (s[3]) blendPixel(d, s[0], s[1], s[2], s[3]);
    }
}

// Solid rectangle (colour 0xRRGGBB, alpha 0..255) over a bitmap / a frame.
inline void fillRect(Bitmap& dst, int x, int y, int w, int h, std::uint32_t rgb, int alpha) {
    Rect r;
    if (dst.empty() || !clipRect(x, y, w, h, dst.w, dst.h, r)) return;
    for (int yy = 0; yy < r.h; ++yy) {
        std::uint8_t* d = dst.bgra.data() + (static_cast<std::size_t>(yy + r.y) * dst.w + r.x) * 4;
        for (int xx = 0; xx < r.w; ++xx, d += 4) blendPixel(d, colB(rgb), colG(rgb), colR(rgb), alpha);
    }
}

inline void fillRect(video::Frame& dst, int x, int y, int w, int h, std::uint32_t rgb, int alpha) {
    Rect r;
    if (!frameValid(dst) || !clipRect(x, y, w, h, dst.width, dst.height, r)) return;
    for (int yy = 0; yy < r.h; ++yy) {
        std::uint8_t* d = dst.data.data() + static_cast<std::size_t>(yy + r.y) * dst.stride + static_cast<std::size_t>(r.x) * 4;
        for (int xx = 0; xx < r.w; ++xx, d += 4) blendPixel(d, colB(rgb), colG(rgb), colR(rgb), alpha);
    }
}

}  // namespace hb::overlay
