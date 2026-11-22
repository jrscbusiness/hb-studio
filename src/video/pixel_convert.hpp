#pragma once
#include <cstddef>
#include <cstdint>

// Pure pixel-format conversions to BGRA8 (top-down, 4 bytes/pixel, alpha forced to 255) from the
// raw formats real-world capture devices commonly deliver, when a DirectShow/Media Foundation
// graph can't (or won't) insert its own colour-space converter before handing frames to this app -
// some camera drivers' filters only expose one of these natively and refuse any other connection
// (DroidCam's DirectShow filter is a known example: see dshow_capture.cpp's caller). No OS API
// dependency: pure byte math, so it's unit-testable without a camera, a driver, or Windows.
namespace hb::video {

// BT.601 full/studio-range YUV -> RGB, clamped to 0..255 (the standard coefficients every one of
// these formats uses; only the byte layout differs between them).
inline void yuvToBgra(int y, int u, int v, std::uint8_t* out) {
    const int c = y - 16, d = u - 128, e = v - 128;
    auto clamp = [](int x) { return x < 0 ? 0 : x > 255 ? 255 : x; };
    out[2] = static_cast<std::uint8_t>(clamp((298 * c + 409 * e + 128) >> 8));          // R
    out[1] = static_cast<std::uint8_t>(clamp((298 * c - 100 * d - 208 * e + 128) >> 8));  // G
    out[0] = static_cast<std::uint8_t>(clamp((298 * c + 516 * d + 128) >> 8));          // B
    out[3] = 255;
}

// YUY2 (a.k.a. YUYV): packed 4:2:2, byte order per 2 pixels: Y0 U Y1 V. `stride` is the source's
// own row stride in bytes (>= width*2); dst is tightly packed BGRA8, width*height*4 bytes.
void convertYuy2ToBgra(const std::uint8_t* src, int width, int height, int stride, std::uint8_t* dst);

// UYVY: same as YUY2 but with U/V and Y swapped within each pair: U0 Y0 V0 Y1.
void convertUyvyToBgra(const std::uint8_t* src, int width, int height, int stride, std::uint8_t* dst);

// NV12: planar 4:2:0 - `width*height` bytes of Y (row stride `strideY`), followed by an
// interleaved U,V plane at half resolution in each dimension (row stride `strideUV`, normally
// equal to strideY). One UV sample is shared by a 2x2 block of Y samples.
void convertNv12ToBgra(const std::uint8_t* y, int strideY, const std::uint8_t* uv, int strideUV, int width,
                       int height, std::uint8_t* dst);

}  // namespace hb::video
