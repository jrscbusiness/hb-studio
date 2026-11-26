// The table-driven YUV -> BGRA converters must produce exactly the bytes of the original per-pixel math.
#include <gtest/gtest.h>

#include <random>
#include <vector>

#include "video/pixel_convert.hpp"
#include "video_ref_convert.hpp"

using namespace hb::video;

namespace {
std::mt19937 g_rng(777);
std::vector<std::uint8_t> randomBytes(std::size_t n) {
    std::vector<std::uint8_t> v(n);
    for (auto& b : v) b = static_cast<std::uint8_t>(g_rng());
    return v;
}
}  // namespace

TEST(PixelConvertPerf, EveryYuvCombinationMatchesYuvToBgra) {
    // One row of 256 pixels per (u,v) pair, luma sweeping 0..255: all 2^24 (y,u,v) combinations, through the
    // vector loop (blocks of 8 pixels) of every converter, compared with the scalar yuvToBgra().
    const int W = 256;
    std::vector<std::uint8_t> yuy2(W * 2), uyvy(W * 2), yPlane(W), uvPlane(W), got(W * 4), want(W * 4);
    for (int i = 0; i < W; ++i) yPlane[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(i);
    for (int u = 0; u < 256; ++u)
        for (int v = 0; v < 256; ++v) {
            for (int p = 0; p < W / 2; ++p) {
                const std::size_t o = static_cast<std::size_t>(p) * 4;
                yuy2[o] = static_cast<std::uint8_t>(2 * p); yuy2[o + 1] = static_cast<std::uint8_t>(u);
                yuy2[o + 2] = static_cast<std::uint8_t>(2 * p + 1); yuy2[o + 3] = static_cast<std::uint8_t>(v);
                uyvy[o] = static_cast<std::uint8_t>(u); uyvy[o + 1] = static_cast<std::uint8_t>(2 * p);
                uyvy[o + 2] = static_cast<std::uint8_t>(v); uyvy[o + 3] = static_cast<std::uint8_t>(2 * p + 1);
                uvPlane[static_cast<std::size_t>(p) * 2] = static_cast<std::uint8_t>(u);
                uvPlane[static_cast<std::size_t>(p) * 2 + 1] = static_cast<std::uint8_t>(v);
            }
            for (int i = 0; i < W; ++i) yuvToBgra(i, u, v, want.data() + static_cast<std::size_t>(i) * 4);
            convertYuy2ToBgra(yuy2.data(), W, 1, W * 2, got.data());
            ASSERT_EQ(got, want) << "YUY2 u=" << u << " v=" << v;
            convertUyvyToBgra(uyvy.data(), W, 1, W * 2, got.data());
            ASSERT_EQ(got, want) << "UYVY u=" << u << " v=" << v;
            convertNv12ToBgra(yPlane.data(), W, uvPlane.data(), W, W, 1, got.data());
            ASSERT_EQ(got, want) << "NV12 u=" << u << " v=" << v;
        }
}

TEST(PixelConvertPerf, Yuy2AndUyvyMatchReferenceOnRandomFramesAllSizes) {
    for (int w : {1, 2, 3, 4, 5, 16, 17, 31, 64, 129, 640})
        for (int h : {1, 2, 3, 9, 48})
            for (int pad : {0, 6}) {
                const int stride = w * 2 + pad;
                auto src = randomBytes(static_cast<std::size_t>(stride) * h + 8);
                std::vector<std::uint8_t> a(static_cast<std::size_t>(w) * h * 4, 0x5A), b = a;
                convertYuy2ToBgra(src.data(), w, h, stride, a.data());
                ref::convertYuy2ToBgra(src.data(), w, h, stride, b.data());
                ASSERT_EQ(a, b) << "YUY2 " << w << "x" << h << " pad " << pad;
                convertUyvyToBgra(src.data(), w, h, stride, a.data());
                ref::convertUyvyToBgra(src.data(), w, h, stride, b.data());
                ASSERT_EQ(a, b) << "UYVY " << w << "x" << h << " pad " << pad;
            }
}

TEST(PixelConvertPerf, Nv12MatchesReferenceOnRandomFramesAllSizes) {
    for (int w : {1, 2, 3, 4, 5, 16, 17, 31, 64, 129, 640})
        for (int h : {1, 2, 3, 9, 48})
            for (int pad : {0, 4}) {
                const int strideY = w + pad, strideUV = (w + 1) / 2 * 2 + pad;
                auto y = randomBytes(static_cast<std::size_t>(strideY) * h + 8);
                auto uv = randomBytes(static_cast<std::size_t>(strideUV) * ((h + 1) / 2) + 8);
                std::vector<std::uint8_t> a(static_cast<std::size_t>(w) * h * 4, 0x33), b = a;
                convertNv12ToBgra(y.data(), strideY, uv.data(), strideUV, w, h, a.data());
                ref::convertNv12ToBgra(y.data(), strideY, uv.data(), strideUV, w, h, b.data());
                ASSERT_EQ(a, b) << "NV12 " << w << "x" << h << " pad " << pad;
            }
}

TEST(PixelConvertPerf, NullAndEmptyInputsAreNoOps) {
    std::vector<std::uint8_t> d(16, 7);
    convertYuy2ToBgra(nullptr, 2, 2, 4, d.data());
    convertNv12ToBgra(nullptr, 2, nullptr, 2, 2, 2, d.data());
    convertUyvyToBgra(d.data(), 0, 2, 4, d.data());
    EXPECT_EQ(d[0], 7);
}
