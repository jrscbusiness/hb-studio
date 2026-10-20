#pragma once
#include <algorithm>
#include <cstdint>

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

// out = a * (1 - k) + b * k, k in [0, 256]. All frames must share one size.
inline void mixFrames(const Frame& a, const Frame& b, Frame& out, int k256) {
    prepareFrame(out, a.width, a.height);
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
inline void downscaleBox(const Frame& src, Frame& dst, int dw, int dh) {
    prepareFrame(dst, dw, dh);
    for (int y = 0; y < dh; ++y) {
        const int y0 = y * src.height / dh;
        const int y1 = std::max(y0 + 1, (y + 1) * src.height / dh);
        for (int x = 0; x < dw; ++x) {
            const int x0 = x * src.width / dw;
            const int x1 = std::max(x0 + 1, (x + 1) * src.width / dw);
            unsigned sum[4] = {0, 0, 0, 0};
            for (int sy = y0; sy < y1; ++sy) {
                const std::uint8_t* row = src.data.data() + static_cast<std::size_t>(sy) * src.stride;
                for (int sx = x0; sx < x1; ++sx)
                    for (int c = 0; c < 4; ++c) sum[c] += row[sx * 4 + c];
            }
            const unsigned cnt = static_cast<unsigned>((y1 - y0) * (x1 - x0));
            std::uint8_t* o = dst.data.data() + static_cast<std::size_t>(y) * dst.stride + x * 4;
            for (int c = 0; c < 4; ++c) o[c] = static_cast<std::uint8_t>(sum[c] / cnt);
        }
    }
}

}  // namespace hb::video
