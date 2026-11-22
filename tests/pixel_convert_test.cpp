#include <gtest/gtest.h>

#include <vector>

#include "video/pixel_convert.hpp"

using namespace hb::video;

namespace {
// The exact same BT.601 math the converters use, so tests assert against first principles rather
// than copy-pasted magic numbers.
void expectedBgra(int y, int u, int v, std::uint8_t out[4]) { yuvToBgra(y, u, v, out); }
}  // namespace

TEST(PixelConvert, YuvToBgraBlackAndWhiteStudioRange) {
    std::uint8_t black[4], white[4];
    yuvToBgra(16, 128, 128, black);    // Y=16 is "black" in studio (limited) range
    yuvToBgra(235, 128, 128, white);   // Y=235 is "white"
    EXPECT_EQ(black[0], 0); EXPECT_EQ(black[1], 0); EXPECT_EQ(black[2], 0); EXPECT_EQ(black[3], 255);
    EXPECT_GE(white[0], 250); EXPECT_GE(white[1], 250); EXPECT_GE(white[2], 250);
}

TEST(PixelConvert, YuvToBgraNeutralGrayHasEqualChannels) {
    std::uint8_t px[4];
    yuvToBgra(126, 128, 128, px);   // u=v=128 (neutral chroma): R==G==B always
    EXPECT_EQ(px[0], px[1]);
    EXPECT_EQ(px[1], px[2]);
}

TEST(Yuy2ToBgra, UniformFrameProducesUniformOutput) {
    const int w = 4, h = 2;
    std::vector<std::uint8_t> src(static_cast<std::size_t>(w) * 2 * h);
    for (int row = 0; row < h; ++row)
        for (int p = 0; p < w / 2; ++p) {
            std::uint8_t* s = src.data() + static_cast<std::size_t>(row) * w * 2 + p * 4;
            s[0] = 100; s[1] = 90; s[2] = 100; s[3] = 160;   // Y U Y V
        }
    std::vector<std::uint8_t> dst(static_cast<std::size_t>(w) * h * 4);
    convertYuy2ToBgra(src.data(), w, h, w * 2, dst.data());
    std::uint8_t expected[4];
    expectedBgra(100, 90, 160, expected);
    for (int i = 0; i < w * h; ++i)
        for (int c = 0; c < 4; ++c)
            EXPECT_EQ(dst[static_cast<std::size_t>(i) * 4 + c], expected[c]) << "pixel " << i << " channel " << c;
}

TEST(Yuy2ToBgra, RespectsStrideLargerThanTightRowSize) {
    const int w = 2, h = 2, stride = 16;   // padded rows, much wider than w*2=4 bytes needed
    std::vector<std::uint8_t> src(static_cast<std::size_t>(stride) * h, 0xCD);   // poison padding
    // row 0: Y=16 (black); row 1: Y=235 (white) - both neutral chroma
    src[0] = 16; src[1] = 128; src[2] = 16; src[3] = 128;
    src[stride + 0] = 235; src[stride + 1] = 128; src[stride + 2] = 235; src[stride + 3] = 128;
    std::vector<std::uint8_t> dst(static_cast<std::size_t>(w) * h * 4);
    convertYuy2ToBgra(src.data(), w, h, stride, dst.data());
    EXPECT_EQ(dst[0], 0); EXPECT_EQ(dst[1], 0); EXPECT_EQ(dst[2], 0);         // row 0 pixel 0: black
    EXPECT_GE(dst[(static_cast<std::size_t>(w) * 4)], 250);                  // row 1 pixel 0: white (B channel)
}

TEST(Yuy2ToBgra, OddWidthDoesNotOverrunTheBuffer) {
    const int w = 3, h = 1;   // one leftover pixel after the single full pair
    std::vector<std::uint8_t> src(4, 0);
    src[0] = 100; src[1] = 128; src[2] = 150; src[3] = 128;
    std::vector<std::uint8_t> dst(static_cast<std::size_t>(w) * h * 4, 0xAA);
    EXPECT_NO_THROW(convertYuy2ToBgra(src.data(), w, h, 4, dst.data()));
    EXPECT_EQ(dst[3], 255);   // alpha of every written pixel, including the leftover one, is opaque
    EXPECT_EQ(dst[11], 255);
}

TEST(UyvyToBgra, SameColoursAsYuy2WithBytesSwapped) {
    const int w = 2, h = 1;
    std::uint8_t yuy2[4] = {100, 90, 110, 160};   // Y0 U Y1 V
    std::uint8_t uyvy[4] = {90, 100, 160, 110};   // U  Y0 V  Y1 (same two pixels)
    std::uint8_t dstA[8], dstB[8];
    convertYuy2ToBgra(yuy2, w, h, w * 2, dstA);
    convertUyvyToBgra(uyvy, w, h, w * 2, dstB);
    for (int i = 0; i < 8; ++i) EXPECT_EQ(dstA[i], dstB[i]) << "byte " << i;
}

TEST(Nv12ToBgra, ChromaIsSharedAcrossEach2x2LumaBlock) {
    // A single 2x2 luma block (one shared chroma pair) with FOUR different luma values at its
    // corners: every pixel must use its own luma but the exact same (non-neutral) chroma.
    const int w = 2, h = 2;
    std::vector<std::uint8_t> yPlane = {16, 90, 180, 235};   // top-left, top-right, bottom-left, bottom-right
    std::vector<std::uint8_t> uvPlane = {200, 90};           // one shared U,V pair for the whole block
    std::vector<std::uint8_t> dst(static_cast<std::size_t>(w) * h * 4);
    convertNv12ToBgra(yPlane.data(), w, uvPlane.data(), w, w, h, dst.data());
    auto at = [&](int x, int y) { return dst.data() + (static_cast<std::size_t>(y) * w + x) * 4; };
    const int corners[4] = {16, 90, 180, 235};
    const int xy[4][2] = {{0, 0}, {1, 0}, {0, 1}, {1, 1}};
    for (int i = 0; i < 4; ++i) {
        std::uint8_t expected[4];
        expectedBgra(corners[i], 200, 90, expected);
        const auto* p = at(xy[i][0], xy[i][1]);
        EXPECT_EQ(p[0], expected[0]) << "corner " << i << " B";
        EXPECT_EQ(p[1], expected[1]) << "corner " << i << " G";
        EXPECT_EQ(p[2], expected[2]) << "corner " << i << " R";
    }
    // Different corners must NOT all collapse to the same colour (that would mean luma is being
    // ignored, e.g. a stride/indexing bug reading the same Y byte for every pixel).
    EXPECT_NE(at(0, 0)[0], at(1, 1)[0]);
}

TEST(Nv12ToBgra, RespectsIndependentLumaAndChromaStrides) {
    const int w = 2, h = 2, strideY = 8, strideUV = 8;
    std::vector<std::uint8_t> yPlane(static_cast<std::size_t>(strideY) * h, 0x11);
    yPlane[0] = 16; yPlane[1] = 16; yPlane[strideY] = 235; yPlane[strideY + 1] = 235;
    std::vector<std::uint8_t> uvPlane(static_cast<std::size_t>(strideUV), 128);
    std::vector<std::uint8_t> dst(static_cast<std::size_t>(w) * h * 4);
    EXPECT_NO_THROW(convertNv12ToBgra(yPlane.data(), strideY, uvPlane.data(), strideUV, w, h, dst.data()));
    EXPECT_EQ(dst[0], 0);                                 // row 0: black
    EXPECT_GE(dst[static_cast<std::size_t>(w) * 4], 250);  // row 1: white
}

TEST(PixelConvert, NullOrDegenerateInputsNeverCrash) {
    std::uint8_t buf[16] = {0};
    EXPECT_NO_THROW(convertYuy2ToBgra(nullptr, 4, 4, 8, buf));
    EXPECT_NO_THROW(convertYuy2ToBgra(buf, 4, 4, 8, nullptr));
    EXPECT_NO_THROW(convertYuy2ToBgra(buf, 0, 0, 0, buf));
    EXPECT_NO_THROW(convertUyvyToBgra(nullptr, 4, 4, 8, buf));
    EXPECT_NO_THROW(convertNv12ToBgra(nullptr, 4, buf, 4, 4, 4, buf));
    EXPECT_NO_THROW(convertNv12ToBgra(buf, 4, nullptr, 4, 4, 4, buf));
    EXPECT_NO_THROW(convertNv12ToBgra(buf, 4, buf, 4, -1, -1, buf));
}
