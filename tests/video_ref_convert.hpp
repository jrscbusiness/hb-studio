#pragma once
// Reference (pre-optimisation) YUV -> BGRA converters, kept verbatim for the equality tests in
// tests/perf_pixel_convert_test.cpp. Do not "improve" this file.
#include <cstddef>
#include <cstdint>

#include "video/pixel_convert.hpp"

namespace hb::video::ref {

inline void convertYuy2ToBgra(const std::uint8_t* src, int width, int height, int stride, std::uint8_t* dst) {
    if (!src || !dst || width <= 0 || height <= 0) return;
    const int pairs = width / 2;
    for (int row = 0; row < height; ++row) {
        const std::uint8_t* s = src + static_cast<std::size_t>(row) * stride;
        std::uint8_t* d = dst + static_cast<std::size_t>(row) * width * 4;
        for (int p = 0; p < pairs; ++p) {
            const int y0 = s[0], u = s[1], y1 = s[2], v = s[3];
            yuvToBgra(y0, u, v, d);
            yuvToBgra(y1, u, v, d + 4);
            s += 4;
            d += 8;
        }
        if (width & 1) {   // odd width: one leftover pixel, reusing the last U/V pair
            yuvToBgra(s[0], s[1], s[3], d);
        }
    }
}

inline void convertUyvyToBgra(const std::uint8_t* src, int width, int height, int stride, std::uint8_t* dst) {
    if (!src || !dst || width <= 0 || height <= 0) return;
    const int pairs = width / 2;
    for (int row = 0; row < height; ++row) {
        const std::uint8_t* s = src + static_cast<std::size_t>(row) * stride;
        std::uint8_t* d = dst + static_cast<std::size_t>(row) * width * 4;
        for (int p = 0; p < pairs; ++p) {
            const int u = s[0], y0 = s[1], v = s[2], y1 = s[3];
            yuvToBgra(y0, u, v, d);
            yuvToBgra(y1, u, v, d + 4);
            s += 4;
            d += 8;
        }
        if (width & 1) {
            yuvToBgra(s[1], s[0], s[2], d);
        }
    }
}

inline void convertNv12ToBgra(const std::uint8_t* y, int strideY, const std::uint8_t* uv, int strideUV, int width,
                              int height, std::uint8_t* dst) {
    if (!y || !uv || !dst || width <= 0 || height <= 0) return;
    for (int row = 0; row < height; ++row) {
        const std::uint8_t* yRow = y + static_cast<std::size_t>(row) * strideY;
        const std::uint8_t* uvRow = uv + static_cast<std::size_t>(row / 2) * strideUV;
        std::uint8_t* d = dst + static_cast<std::size_t>(row) * width * 4;
        for (int col = 0; col < width; ++col) {
            const int u = uvRow[(col / 2) * 2];
            const int v = uvRow[(col / 2) * 2 + 1];
            yuvToBgra(yRow[col], u, v, d);
            d += 4;
        }
    }
}

}  // namespace hb::video::ref
