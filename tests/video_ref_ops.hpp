#pragma once
// Reference (pre-optimisation) implementations of the video hot paths, kept verbatim so the unit tests
// can prove the optimised versions in src/ produce the same bytes. Do not "improve" this file.
#include <algorithm>
#include <cstdint>
#include <vector>

#include "video/frame_ops.hpp"

namespace hb::video::ref {

// out = a * (1 - k) + b * k, k in [0, 256]. All frames must share one size.
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
inline void downscaleBox(const Frame& src, Frame& dst, int dw, int dh) {
    if (dw <= 0 || dh <= 0) return;
    prepareFrame(dst, dw, dh);
    if (src.width <= 0 || src.height <= 0 ||
        src.data.size() < static_cast<std::size_t>(src.stride) * static_cast<std::size_t>(src.height))
        return;
    for (int y = 0; y < dh; ++y) {
        const int y0 = y * src.height / dh;
        const int y1 = std::max(y0 + 1, (y + 1) * src.height / dh);
        for (int x = 0; x < dw; ++x) {
            const int x0 = x * src.width / dw;
            const int x1 = std::max(x0 + 1, (x + 1) * src.width / dw);
            std::uint64_t sum[4] = {0, 0, 0, 0};
            for (int sy = y0; sy < y1; ++sy) {
                const std::uint8_t* row = src.data.data() + static_cast<std::size_t>(sy) * src.stride;
                for (int sx = x0; sx < x1; ++sx)
                    for (int c = 0; c < 4; ++c) sum[c] += row[sx * 4 + c];
            }
            const std::uint64_t cnt = static_cast<std::uint64_t>(y1 - y0) * static_cast<std::uint64_t>(x1 - x0);
            std::uint8_t* o = dst.data.data() + static_cast<std::size_t>(y) * dst.stride + x * 4;
            for (int c = 0; c < 4; ++c) o[c] = static_cast<std::uint8_t>(sum[c] / cnt);
        }
    }
}

// Scales `src` into `dst` keeping the aspect ratio (black bars). Nearest-neighbour: cheap enough for
// 1080p Program output. Safe for any size, including degenerate ones. Output alpha is always 255.
inline void fitFrame(const Frame& src, Frame& dst) {
    if (dst.width <= 0 || dst.height <= 0 || dst.data.size() < static_cast<std::size_t>(dst.stride) * dst.height) return;
    for (std::size_t i = 0; i + 3 < dst.data.size(); i += 4) {
        dst.data[i] = dst.data[i + 1] = dst.data[i + 2] = 0;
        dst.data[i + 3] = 255;
    }
    if (src.width <= 0 || src.height <= 0 ||
        src.data.size() < static_cast<std::size_t>(src.stride) * static_cast<std::size_t>(src.height))
        return;
    const double scale = std::min(static_cast<double>(dst.width) / src.width, static_cast<double>(dst.height) / src.height);
    const int tw = std::max(1, std::min(dst.width, static_cast<int>(src.width * scale + 0.5)));
    const int th = std::max(1, std::min(dst.height, static_cast<int>(src.height * scale + 0.5)));
    const int ox = (dst.width - tw) / 2, oy = (dst.height - th) / 2;
    std::vector<int> xmap(static_cast<std::size_t>(tw));
    for (int x = 0; x < tw; ++x) xmap[static_cast<std::size_t>(x)] = std::min(src.width - 1, static_cast<int>(static_cast<std::int64_t>(x) * src.width / tw)) * 4;
    for (int y = 0; y < th; ++y) {
        const int sy = std::min(src.height - 1, static_cast<int>(static_cast<std::int64_t>(y) * src.height / th));
        const std::uint8_t* srow = src.data.data() + static_cast<std::size_t>(sy) * src.stride;
        std::uint8_t* drow = dst.data.data() + static_cast<std::size_t>(oy + y) * dst.stride + static_cast<std::size_t>(ox) * 4;
        for (int x = 0; x < tw; ++x) {
            const std::uint8_t* sp = srow + xmap[static_cast<std::size_t>(x)];
            std::uint8_t* dp = drow + static_cast<std::size_t>(x) * 4;
            dp[0] = sp[0]; dp[1] = sp[1]; dp[2] = sp[2]; dp[3] = 255;
        }
    }
}

}  // namespace hb::video::ref
