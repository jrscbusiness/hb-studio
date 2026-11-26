// The branch-free opaque-row paths of overlay::blendBitmap / fillRect must equal the original per-pixel code.
#include <gtest/gtest.h>

#include <array>
#include <random>

#include "overlay/blend.hpp"
#include "video/frame_ops.hpp"

using namespace hb;

namespace {

std::mt19937 g_rng(99);

// The original implementations (pre-optimisation), verbatim.
void refBlendBitmap(video::Frame& dst, const overlay::Bitmap& src, int dx, int dy, double opacity) {
    using namespace overlay;
    if (src.empty() || !frameValid(dst)) return;
    const int op = static_cast<int>(std::clamp(opacity, 0.0, 1.0) * 256.0 + 0.5);
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

void refFillRect(video::Frame& dst, int x, int y, int w, int h, std::uint32_t rgb, int alpha) {
    using namespace overlay;
    Rect r;
    if (!frameValid(dst) || !clipRect(x, y, w, h, dst.width, dst.height, r)) return;
    for (int yy = 0; yy < r.h; ++yy) {
        std::uint8_t* d = dst.data.data() + static_cast<std::size_t>(yy + r.y) * dst.stride + static_cast<std::size_t>(r.x) * 4;
        for (int xx = 0; xx < r.w; ++xx, d += 4) blendPixel(d, colB(rgb), colG(rgb), colR(rgb), alpha);
    }
}

video::Frame randomFrame(int w, int h, int translucentRows) {
    video::Frame f;
    video::prepareFrame(f, w, h);
    for (auto& v : f.data) v = static_cast<std::uint8_t>(g_rng());
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            std::uint8_t& a = f.data[(static_cast<std::size_t>(y) * w + x) * 4 + 3];
            a = (y < translucentRows && (g_rng() & 1)) ? static_cast<std::uint8_t>(g_rng()) : 255;
        }
    return f;
}

overlay::Bitmap randomBitmap(int w, int h) {
    overlay::Bitmap b;
    b.resize(w, h);
    for (auto& v : b.bgra) v = static_cast<std::uint8_t>(g_rng());
    for (std::size_t i = 3; i < b.bgra.size(); i += 4) {
        const unsigned k = g_rng() % 6;
        b.bgra[i] = k == 0 ? 0 : k == 1 ? 255 : static_cast<std::uint8_t>(g_rng());
    }
    return b;
}

}  // namespace

TEST(BlendPerf, BlendBitmapMatchesOriginal) {
    int n = 0;
    for (int translucent : {0, 3})   // 0: every row opaque (fast path), 3: first rows have translucent pixels (slow path)
        for (double opacity : {1.0, 0.6, 0.999, 0.01, 0.0, 0.5})
            for (const auto& pos : {std::pair{0, 0}, std::pair{5, 2}, std::pair{-7, -3}, std::pair{30, 20}, std::pair{50, 40}, std::pair{-100, 0}}) {
                video::Frame a = randomFrame(64, 36, translucent), b = a;
                const overlay::Bitmap bm = randomBitmap(40, 25);
                overlay::blendBitmap(a, bm, pos.first, pos.second, opacity);
                refBlendBitmap(b, bm, pos.first, pos.second, opacity);
                ASSERT_EQ(a.data, b.data) << "opacity " << opacity << " at " << pos.first << "," << pos.second << " tr " << translucent;
                ++n;
            }
    EXPECT_EQ(n, 72);
}

TEST(BlendPerf, BlendBitmapEveryAlphaAndChannelValue) {
    // All (src, dst, alpha) triples for one channel, with and without the extra opacity.
    overlay::Bitmap bm;
    bm.resize(256, 1);
    for (double opacity : {1.0, 0.37}) {
        for (int a = 0; a < 256; ++a) {
            video::Frame f1, f2;
            video::prepareFrame(f1, 256, 1);
            for (int x = 0; x < 256; ++x) {
                std::uint8_t* p = &bm.bgra[static_cast<std::size_t>(x) * 4];
                p[0] = static_cast<std::uint8_t>(x); p[1] = static_cast<std::uint8_t>(255 - x); p[2] = static_cast<std::uint8_t>(x ^ 0x55);
                p[3] = static_cast<std::uint8_t>(a);
                std::uint8_t* d = &f1.data[static_cast<std::size_t>(x) * 4];
                d[0] = static_cast<std::uint8_t>(255 - x); d[1] = static_cast<std::uint8_t>(x); d[2] = static_cast<std::uint8_t>(x * 7); d[3] = 255;
            }
            f2 = f1;
            overlay::blendBitmap(f1, bm, 0, 0, opacity);
            refBlendBitmap(f2, bm, 0, 0, opacity);
            ASSERT_EQ(f1.data, f2.data) << "alpha " << a << " opacity " << opacity;
        }
    }
}

TEST(BlendPerf, FillRectMatchesOriginal) {
    for (int translucent : {0, 4})
        for (int alpha : {-5, 0, 1, 77, 120, 254, 255, 256, 400})
            for (const auto& r : {std::array<int, 4>{0, 0, 64, 36}, {3, 5, 20, 10}, {-4, -4, 20, 20}, {50, 30, 100, 100}, {10, 10, 0, 5}}) {
                video::Frame a = randomFrame(64, 36, translucent), b = a;
                overlay::fillRect(a, r[0], r[1], r[2], r[3], 0x2A4B6C, alpha);
                refFillRect(b, r[0], r[1], r[2], r[3], 0x2A4B6C, alpha);
                ASSERT_EQ(a.data, b.data) << "alpha " << alpha;
            }
}
