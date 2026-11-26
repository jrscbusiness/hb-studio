#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

#include "video/program_output.hpp"

namespace hb::video {

// Allocates (or resizes) a BGRA8 frame; keeps the buffer when the size matches.
inline void prepareFrame(Frame& f, int width, int height) {
    if (f.width == width && f.height == height && !f.data.empty()) return;
    f.width = width;
    f.height = height;
    f.stride = width * 4;
    f.format = PixelFormat::BGRA8;
    f.data.assign(static_cast<std::size_t>(f.stride) * height, 0);
}

namespace detail {

// A BGRA pixel as one 32-bit word, independent of the host byte order (memcpy of the byte pattern).
inline std::uint32_t packBgra(std::uint8_t b, std::uint8_t g, std::uint8_t r, std::uint8_t a) {
    const std::uint8_t px[4] = {b, g, r, a};
    std::uint32_t v;
    std::memcpy(&v, px, 4);
    return v;
}
inline std::uint32_t load32(const std::uint8_t* p) { std::uint32_t v; std::memcpy(&v, p, 4); return v; }
inline void store32(std::uint8_t* p, std::uint32_t v) { std::memcpy(p, &v, 4); }

inline void fillPixels(std::uint8_t* p, std::size_t pixels, std::uint32_t v) {
    for (std::size_t i = 0; i < pixels; ++i) store32(p + i * 4, v);
}

// Copies `n` pixels forcing alpha to 255 (auto-vectorises: no branches, 32-bit lanes).
inline void copyRowOpaque(std::uint8_t* d, const std::uint8_t* s, std::size_t n, std::uint32_t alphaMask) {
    for (std::size_t i = 0; i < n; ++i) store32(d + i * 4, load32(s + i * 4) | alphaMask);
}

// Per-thread scratch for the scalers: steady-state frames allocate nothing.
struct ScaleScratch {
    int fitSrcW = -1, fitDstW = -1;
    std::vector<std::uint32_t> xmap;       // fitFrame: source pixel index per destination column
    int boxSrcW = -1, boxDstW = -1;
    std::vector<std::uint32_t> bx0, bx1;   // downscaleBox: source column range per destination column
    std::vector<std::uint32_t> acc;        // downscaleBox: vertical sums, 4 per source column
};
inline ScaleScratch& scaleScratch() {
    thread_local ScaleScratch s;
    return s;
}

}  // namespace detail

// out = a * (1 - k) + b * k, k in [0, 256]. All frames must share one size.
// (Memory-bound: a 16-bit-lane variant measured no faster at 1080p, so the plain loop stays - docs/DESEMPENHO.md.)
inline void mixFrames(const Frame& a, const Frame& b, Frame& out, int k256) {
    prepareFrame(out, a.width, a.height);
    if (b.width != a.width || b.height != a.height || a.data.size() != out.data.size() || b.data.size() != out.data.size()) {
        out.data = a.data;  // mismatched sizes: never read/write out of bounds
        return;
    }
    k256 = std::clamp(k256, 0, 256);
    const int ik = 256 - k256;
    const std::size_t n = out.data.size();
    const std::uint8_t* pa = a.data.data();
    const std::uint8_t* pb = b.data.data();
    std::uint8_t* po = out.data.data();
    for (std::size_t i = 0; i < n; ++i)
        po[i] = static_cast<std::uint8_t>((pa[i] * ik + pb[i] * k256 + 128) >> 8);
}

// Box-filter downscale (any ratio, area average over the covered source pixels).
// Two passes: the source rows of each destination row are summed vertically (a plain widening add the
// compiler vectorises), then each destination pixel sums its source columns and divides by the box area
// with an exact multiply-shift (floor(n / d) == (n * ceil(2^32 / d)) >> 32 whenever n * d < 2^32).
inline void downscaleBox(const Frame& src, Frame& dst, int dw, int dh) {
    if (dw <= 0 || dh <= 0) return;
    prepareFrame(dst, dw, dh);
    if (src.width <= 0 || src.height <= 0 || src.stride < src.width * 4 ||
        src.data.size() < static_cast<std::size_t>(src.stride) * static_cast<std::size_t>(src.height))
        return;
    const int sw = src.width, sh = src.height;
    detail::ScaleScratch& sc = detail::scaleScratch();
    if (sc.boxSrcW != sw || sc.boxDstW != dw) {
        sc.bx0.resize(static_cast<std::size_t>(dw));
        sc.bx1.resize(static_cast<std::size_t>(dw));
        for (int x = 0; x < dw; ++x) {
            const std::int64_t x0 = static_cast<std::int64_t>(x) * sw / dw;
            const std::int64_t x1 = std::max<std::int64_t>(x0 + 1, static_cast<std::int64_t>(x + 1) * sw / dw);
            sc.bx0[static_cast<std::size_t>(x)] = static_cast<std::uint32_t>(x0);
            sc.bx1[static_cast<std::size_t>(x)] = static_cast<std::uint32_t>(x1);
        }
        sc.boxSrcW = sw;
        sc.boxDstW = dw;
    }
    const std::size_t accN = static_cast<std::size_t>(sw) * 4;
    if (sc.acc.size() < accN) sc.acc.resize(accN);
    std::uint32_t* acc = sc.acc.data();
    // Column widths take at most two values (f, f+1) when downscaling, and 1 when upscaling.
    const std::uint64_t colMin = std::max<std::uint64_t>(1, static_cast<std::uint64_t>(sw / dw));
    for (int y = 0; y < dh; ++y) {
        const int y0 = static_cast<int>(static_cast<std::int64_t>(y) * sh / dh);
        const int y1 = std::max(y0 + 1, static_cast<int>(static_cast<std::int64_t>(y + 1) * sh / dh));
        {
            const std::uint8_t* r0 = src.data.data() + static_cast<std::size_t>(y0) * src.stride;
            for (std::size_t i = 0; i < accN; ++i) acc[i] = r0[i];
        }
        for (int sy = y0 + 1; sy < y1; ++sy) {
            const std::uint8_t* row = src.data.data() + static_cast<std::size_t>(sy) * src.stride;
            for (std::size_t i = 0; i < accN; ++i) acc[i] += row[i];
        }
        const std::uint64_t rows = static_cast<std::uint64_t>(y1 - y0);
        // Reciprocals for the two possible box areas of this row (area = rows * column width).
        std::uint64_t m[2] = {0, 0};
        const std::uint64_t area[2] = {rows * colMin, rows * (colMin + 1)};
        for (int k = 0; k < 2; ++k)
            if (area[k] < 4104) m[k] = ((std::uint64_t{1} << 32) + area[k] - 1) / area[k];
        std::uint8_t* o = dst.data.data() + static_cast<std::size_t>(y) * dst.stride;
        for (int x = 0; x < dw; ++x, o += 4) {
            const std::uint32_t x0 = sc.bx0[static_cast<std::size_t>(x)], x1 = sc.bx1[static_cast<std::size_t>(x)];
            std::uint64_t s0 = 0, s1 = 0, s2 = 0, s3 = 0;
            const std::uint32_t* a = acc + static_cast<std::size_t>(x0) * 4;
            for (std::uint32_t sx = x0; sx < x1; ++sx, a += 4) { s0 += a[0]; s1 += a[1]; s2 += a[2]; s3 += a[3]; }
            const std::uint64_t cols = x1 - x0, cnt = rows * cols;
            const std::uint64_t mm = (cols == colMin) ? m[0] : (cols == colMin + 1) ? m[1] : 0;
            if (mm) {
                o[0] = static_cast<std::uint8_t>((s0 * mm) >> 32);
                o[1] = static_cast<std::uint8_t>((s1 * mm) >> 32);
                o[2] = static_cast<std::uint8_t>((s2 * mm) >> 32);
                o[3] = static_cast<std::uint8_t>((s3 * mm) >> 32);
            } else {
                o[0] = static_cast<std::uint8_t>(s0 / cnt);
                o[1] = static_cast<std::uint8_t>(s1 / cnt);
                o[2] = static_cast<std::uint8_t>(s2 / cnt);
                o[3] = static_cast<std::uint8_t>(s3 / cnt);
            }
        }
    }
}

// Scales `src` into `dst` keeping the aspect ratio (black bars). Nearest-neighbour: cheap enough for
// 1080p Program output. Safe for any size, including degenerate ones. Output alpha is always 255.
// Only the bars are cleared (the picture overwrites the rest); a 1:1 fit is a row-wise copy and rows that
// repeat a source row (upscaling) are copied from the previous destination row.
inline void fitFrame(const Frame& src, Frame& dst) {
    if (dst.width <= 0 || dst.height <= 0 || dst.data.size() < static_cast<std::size_t>(dst.stride) * dst.height) return;
    const std::uint32_t opaqueBlack = detail::packBgra(0, 0, 0, 255);   // also the "set alpha to 255" mask
    const bool tight = dst.stride == dst.width * 4;
    const bool srcOk = src.width > 0 && src.height > 0 && src.stride >= src.width * 4 &&
                       src.data.size() >= static_cast<std::size_t>(src.stride) * static_cast<std::size_t>(src.height);
    if (!tight || !srcOk) {   // unusual layout / nothing to draw: clear the whole buffer like the original did
        detail::fillPixels(dst.data.data(), dst.data.size() / 4, opaqueBlack);
        if (!srcOk) return;
    }
    const double scale = std::min(static_cast<double>(dst.width) / src.width, static_cast<double>(dst.height) / src.height);
    const int tw = std::max(1, std::min(dst.width, static_cast<int>(src.width * scale + 0.5)));
    const int th = std::max(1, std::min(dst.height, static_cast<int>(src.height * scale + 0.5)));
    const int ox = (dst.width - tw) / 2, oy = (dst.height - th) / 2;
    if (tight) {   // clear only the bars
        std::uint8_t* base = dst.data.data();
        const std::size_t stride = static_cast<std::size_t>(dst.stride);
        detail::fillPixels(base, static_cast<std::size_t>(oy) * static_cast<std::size_t>(dst.width), opaqueBlack);
        detail::fillPixels(base + static_cast<std::size_t>(oy + th) * stride,
                           static_cast<std::size_t>(dst.height - oy - th) * static_cast<std::size_t>(dst.width), opaqueBlack);
        const int rightBar = dst.width - ox - tw;
        if (ox > 0 || rightBar > 0)
            for (int y = oy; y < oy + th; ++y) {
                std::uint8_t* row = base + static_cast<std::size_t>(y) * stride;
                detail::fillPixels(row, static_cast<std::size_t>(ox), opaqueBlack);
                detail::fillPixels(row + static_cast<std::size_t>(ox + tw) * 4, static_cast<std::size_t>(rightBar), opaqueBlack);
            }
    }
    const bool identityX = (tw == src.width);
    const std::uint32_t* xmap = nullptr;
    if (!identityX) {
        detail::ScaleScratch& sc = detail::scaleScratch();
        if (sc.fitSrcW != src.width || sc.fitDstW != tw) {
            sc.xmap.resize(static_cast<std::size_t>(tw));
            for (int x = 0; x < tw; ++x)
                sc.xmap[static_cast<std::size_t>(x)] = static_cast<std::uint32_t>(
                    std::min(src.width - 1, static_cast<int>(static_cast<std::int64_t>(x) * src.width / tw)));
            sc.fitSrcW = src.width;
            sc.fitDstW = tw;
        }
        xmap = sc.xmap.data();
    }
    const std::size_t rowBytes = static_cast<std::size_t>(tw) * 4;
    int prevSy = -1;
    const std::uint8_t* prevRow = nullptr;
    for (int y = 0; y < th; ++y) {
        const int sy = std::min(src.height - 1, static_cast<int>(static_cast<std::int64_t>(y) * src.height / th));
        std::uint8_t* drow = dst.data.data() + static_cast<std::size_t>(oy + y) * dst.stride + static_cast<std::size_t>(ox) * 4;
        if (sy == prevSy && prevRow) {
            std::memcpy(drow, prevRow, rowBytes);   // same source row as the line above: already converted
        } else {
            const std::uint8_t* srow = src.data.data() + static_cast<std::size_t>(sy) * src.stride;
            if (identityX) {
                detail::copyRowOpaque(drow, srow, static_cast<std::size_t>(tw), opaqueBlack);
            } else {
                for (int x = 0; x < tw; ++x)
                    detail::store32(drow + static_cast<std::size_t>(x) * 4,
                                    detail::load32(srow + static_cast<std::size_t>(xmap[x]) * 4) | opaqueBlack);
            }
            prevSy = sy;
            prevRow = drow;
        }
    }
}

}  // namespace hb::video
