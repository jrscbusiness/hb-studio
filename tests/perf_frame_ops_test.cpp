// Proves the optimised frame_ops (fitFrame / downscaleBox / mixFrames) are byte-identical to the original
// implementations kept in tests/video_ref_ops.hpp, on random frames and edge cases.
#include <gtest/gtest.h>

#include <random>

#include "video/frame_ops.hpp"
#include "video_ref_ops.hpp"

using namespace hb::video;

namespace {

std::mt19937 g_rng(12345);

Frame randomFrame(int w, int h, bool opaque = false, int stridePad = 0) {
    Frame f;
    f.width = w;
    f.height = h;
    f.stride = w * 4 + stridePad;
    f.format = PixelFormat::BGRA8;
    f.data.resize(static_cast<std::size_t>(f.stride) * h);
    for (auto& v : f.data) v = static_cast<std::uint8_t>(g_rng());
    if (opaque)
        for (std::size_t i = 3; i < f.data.size(); i += 4) f.data[i] = 255;
    return f;
}

Frame stalePrepared(int w, int h) {   // a destination full of garbage, so "not written" shows up as a diff
    Frame f = randomFrame(w, h);
    return f;
}

void expectSame(const Frame& a, const Frame& b, const std::string& what) {
    ASSERT_EQ(a.width, b.width) << what;
    ASSERT_EQ(a.height, b.height) << what;
    ASSERT_EQ(a.stride, b.stride) << what;
    ASSERT_EQ(a.data.size(), b.data.size()) << what;
    if (a.data != b.data) {
        std::size_t i = 0;
        while (a.data[i] == b.data[i]) ++i;
        FAIL() << what << ": first difference at byte " << i << " (" << int(a.data[i]) << " vs " << int(b.data[i]) << ")";
    }
}

}  // namespace

TEST(FrameOpsPerf, FitFrameMatchesReference) {
    const int sizes[][2] = {{1, 1}, {2, 3}, {7, 5}, {16, 9}, {33, 17}, {64, 48}, {100, 100}, {160, 90}, {96, 54}, {320, 180}};
    int cases = 0;
    for (const auto& s : sizes)
        for (const auto& d : sizes) {
            for (int variant = 0; variant < 3; ++variant) {
                Frame src = randomFrame(s[0], s[1], variant == 1, variant == 2 ? 8 : 0);   // opaque / padded source stride
                Frame a = stalePrepared(d[0], d[1]);
                Frame b = a;
                fitFrame(src, a);
                ref::fitFrame(src, b);
                expectSame(a, b, "fit " + std::to_string(s[0]) + "x" + std::to_string(s[1]) + " -> " + std::to_string(d[0]) + "x" +
                                     std::to_string(d[1]) + " v" + std::to_string(variant));
                ++cases;
            }
        }
    EXPECT_EQ(cases, 300);
}

TEST(FrameOpsPerf, FitFrameCacheSurvivesChangingSizes) {
    // The per-thread column map is cached by (source width, target width): alternate sizes to catch a stale map.
    Frame a1 = randomFrame(40, 30), a2 = randomFrame(40, 30), b1 = randomFrame(64, 36);
    for (int i = 0; i < 4; ++i) {
        Frame d1 = stalePrepared(100, 60), d2 = d1, d3 = stalePrepared(50, 50), d4 = d3;
        fitFrame(a1, d1); ref::fitFrame(a1, d2);
        expectSame(d1, d2, "alt1");
        fitFrame(b1, d3); ref::fitFrame(b1, d4);
        expectSame(d3, d4, "alt2");
    }
}

TEST(FrameOpsPerf, FitFrameDegenerateInputs) {
    Frame dst = stalePrepared(32, 18), want = dst;
    Frame emptySrc;
    fitFrame(emptySrc, dst);
    ref::fitFrame(emptySrc, want);
    expectSame(dst, want, "empty src");
    Frame shortSrc = randomFrame(10, 10);
    shortSrc.data.resize(100);   // fewer bytes than stride*height
    dst = stalePrepared(32, 18);
    want = dst;
    fitFrame(shortSrc, dst);
    ref::fitFrame(shortSrc, want);
    expectSame(dst, want, "short src");
    Frame noDst;
    fitFrame(randomFrame(4, 4), noDst);   // must not crash
    SUCCEED();
}

TEST(FrameOpsPerf, FitFramePaddedDestinationStride) {
    Frame src = randomFrame(30, 20);
    Frame a = randomFrame(48, 27, false, 16), b = a;   // stride > width*4: legacy path
    fitFrame(src, a);
    ref::fitFrame(src, b);
    expectSame(a, b, "padded dst");
}

TEST(FrameOpsPerf, DownscaleBoxMatchesReference) {
    const int srcs[][2] = {{1, 1}, {5, 3}, {16, 9}, {33, 17}, {64, 36}, {100, 75}, {160, 90}, {320, 180}, {301, 303}};
    const int dsts[][2] = {{1, 1}, {2, 2}, {3, 5}, {8, 4}, {16, 9}, {32, 18}, {64, 36}, {96, 54}, {100, 100}, {400, 300}, {7, 11}};
    int cases = 0;
    for (const auto& s : srcs)
        for (const auto& d : dsts)
            for (int pad : {0, 12}) {
                Frame src = randomFrame(s[0], s[1], false, pad);
                Frame a, b;
                downscaleBox(src, a, d[0], d[1]);
                ref::downscaleBox(src, b, d[0], d[1]);
                expectSame(a, b, "box " + std::to_string(s[0]) + "x" + std::to_string(s[1]) + " -> " + std::to_string(d[0]) + "x" +
                                     std::to_string(d[1]) + " pad" + std::to_string(pad));
                ++cases;
            }
    EXPECT_EQ(cases, 9 * 11 * 2);
}

TEST(FrameOpsPerf, DownscaleBoxExtremesAndLargeBoxes) {
    // All 255 / all 0: the reciprocal must not round 255 down to 254, nor produce 1 for 0.
    for (std::uint8_t v : {std::uint8_t{0}, std::uint8_t{255}, std::uint8_t{1}, std::uint8_t{254}}) {
        Frame src = randomFrame(240, 135);
        std::fill(src.data.begin(), src.data.end(), v);
        for (const auto& d : {std::pair{192, 108}, std::pair{96, 54}, std::pair{1, 1}, std::pair{7, 5}, std::pair{240, 135}}) {
            Frame a, b;
            downscaleBox(src, a, d.first, d.second);
            ref::downscaleBox(src, b, d.first, d.second);
            expectSame(a, b, "flat " + std::to_string(v));
        }
    }
    // Boxes of > 4104 pixels use the plain division path.
    Frame big = randomFrame(400, 300);
    Frame a, b;
    downscaleBox(big, a, 3, 2);
    ref::downscaleBox(big, b, 3, 2);
    expectSame(a, b, "huge box");
    // Realistic ratios: 1080p -> the four UI presets, plus a 4:3 and an odd camera size.
    Frame hd = randomFrame(1920, 1080);
    for (const auto& d : {std::pair{320, 180}, std::pair{480, 270}, std::pair{640, 360}, std::pair{960, 540}, std::pair{96, 54},
                          std::pair{144, 81}, std::pair{192, 108}, std::pair{256, 144}, std::pair{240, 135}}) {
        Frame x, y;
        downscaleBox(hd, x, d.first, d.second);
        ref::downscaleBox(hd, y, d.first, d.second);
        expectSame(x, y, "1080p -> " + std::to_string(d.first));
    }
    Frame odd = randomFrame(1366, 768);
    Frame x, y;
    downscaleBox(odd, x, 640, 360);
    ref::downscaleBox(odd, y, 640, 360);
    expectSame(x, y, "1366x768");
}

TEST(FrameOpsPerf, DownscaleBoxReusesDestinationAndCaches) {
    Frame s1 = randomFrame(64, 36), s2 = randomFrame(80, 45);
    Frame a, b;
    for (int i = 0; i < 3; ++i) {
        downscaleBox(s1, a, 16, 9); ref::downscaleBox(s1, b, 16, 9);
        expectSame(a, b, "reuse1");
        downscaleBox(s2, a, 16, 9); ref::downscaleBox(s2, b, 16, 9);   // same dw, different source width: cache must rebuild
        expectSame(a, b, "reuse2");
    }
}

TEST(FrameOpsPerf, DownscaleBoxInvalidSourceLeavesDestinationLikeReference) {
    Frame bad;
    Frame a, b;
    downscaleBox(bad, a, 8, 8);
    ref::downscaleBox(bad, b, 8, 8);
    expectSame(a, b, "invalid src");
}

TEST(FrameOpsPerf, MixFramesMatchesReferenceForEveryWeight) {
    Frame a = randomFrame(37, 11), b = randomFrame(37, 11);
    for (int k = -3; k <= 260; ++k) {
        Frame x, y;
        mixFrames(a, b, x, k);
        ref::mixFrames(a, b, y, k);
        expectSame(x, y, "k=" + std::to_string(k));
    }
    // Extremes of the byte range for all weights.
    Frame lo = randomFrame(8, 8), hi = randomFrame(8, 8);
    std::fill(lo.data.begin(), lo.data.end(), 0);
    std::fill(hi.data.begin(), hi.data.end(), 255);
    for (int k = 0; k <= 256; ++k) {
        Frame x, y;
        mixFrames(hi, hi, x, k); ref::mixFrames(hi, hi, y, k);
        expectSame(x, y, "255/255");
        mixFrames(hi, lo, x, k); ref::mixFrames(hi, lo, y, k);
        expectSame(x, y, "255/0");
    }
    Frame small = randomFrame(4, 4), x, y;   // mismatched sizes
    mixFrames(a, small, x, 100);
    ref::mixFrames(a, small, y, 100);
    expectSame(x, y, "mismatch");
}
