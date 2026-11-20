#pragma once
// float32 -> integer PCM conversion shared by the WAV / FLAC / AVI writers.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace hb::rec::detail {

// Maps [-1, 1] to the signed range of `bits` (scale 2^(bits-1), round to nearest, clamp, NaN -> 0).
inline std::int32_t quantizePcm(float x, int bits) {
    const float scale = static_cast<float>(std::int64_t(1) << (bits - 1));
    float v = x * scale;
    if (!(v == v)) return 0;
    const float hi = scale - 1.0f;
    if (v > hi) v = hi;
    if (v < -scale) v = -scale;
    return static_cast<std::int32_t>(std::lrintf(v));
}

// Writes `n` interleaved samples as little-endian PCM (16 / 24 bits) or IEEE float (32).
inline void packPcm(const float* in, std::size_t n, int bits, std::uint8_t* out) {
    if (bits == 32) {
        for (std::size_t i = 0; i < n; ++i) {
            float f = in[i];
            if (!(f == f)) f = 0.0f;
            std::uint32_t u;
            std::memcpy(&u, &f, 4);
            out[0] = static_cast<std::uint8_t>(u);
            out[1] = static_cast<std::uint8_t>(u >> 8);
            out[2] = static_cast<std::uint8_t>(u >> 16);
            out[3] = static_cast<std::uint8_t>(u >> 24);
            out += 4;
        }
    } else if (bits == 24) {
        for (std::size_t i = 0; i < n; ++i) {
            const std::uint32_t u = static_cast<std::uint32_t>(quantizePcm(in[i], 24));
            out[0] = static_cast<std::uint8_t>(u);
            out[1] = static_cast<std::uint8_t>(u >> 8);
            out[2] = static_cast<std::uint8_t>(u >> 16);
            out += 3;
        }
    } else {
        for (std::size_t i = 0; i < n; ++i) {
            const std::uint32_t u = static_cast<std::uint32_t>(quantizePcm(in[i], 16));
            out[0] = static_cast<std::uint8_t>(u);
            out[1] = static_cast<std::uint8_t>(u >> 8);
            out += 2;
        }
    }
}

// Normalises the profile's audioBitsPerSample: 16, 24 or 32 (float).
inline int normalizeBits(int b) { return b <= 16 ? 16 : (b <= 24 ? 24 : 32); }

}  // namespace hb::rec::detail
