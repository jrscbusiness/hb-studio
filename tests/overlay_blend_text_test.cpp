#include <gtest/gtest.h>

#include "overlay/blend.hpp"
#include "overlay/text.hpp"
#include "video/frame_ops.hpp"

using namespace hb::overlay;

namespace {
hb::video::Frame opaqueFrame(int w, int h, std::uint8_t b, std::uint8_t g, std::uint8_t r) {
    hb::video::Frame f;
    hb::video::prepareFrame(f, w, h);
    for (std::size_t i = 0; i < f.data.size(); i += 4) { f.data[i] = b; f.data[i + 1] = g; f.data[i + 2] = r; f.data[i + 3] = 255; }
    return f;
}
const std::uint8_t* px(const hb::video::Frame& f, int x, int y) { return f.data.data() + y * f.stride + x * 4; }
}  // namespace

TEST(OverlayBlend, OpaqueSourceReplaces) {
    auto f = opaqueFrame(4, 4, 10, 20, 30);
    Bitmap b; b.resize(2, 2);
    for (std::size_t i = 0; i < b.bgra.size(); i += 4) { b.bgra[i] = 200; b.bgra[i + 1] = 100; b.bgra[i + 2] = 50; b.bgra[i + 3] = 255; }
    blendBitmap(f, b, 1, 1);
    EXPECT_EQ(px(f, 1, 1)[0], 200); EXPECT_EQ(px(f, 2, 2)[2], 50);
    EXPECT_EQ(px(f, 0, 0)[0], 10);  // outside untouched
    EXPECT_EQ(px(f, 1, 1)[3], 255);
}

TEST(OverlayBlend, StraightAlphaHalf) {
    auto f = opaqueFrame(2, 1, 0, 0, 0);
    Bitmap b; b.resize(1, 1);
    b.bgra = {255, 255, 255, 128};  // straight alpha: colour is full white, coverage 50%
    blendBitmap(f, b, 0, 0);
    EXPECT_NEAR(px(f, 0, 0)[0], 128, 1);
    EXPECT_EQ(px(f, 0, 0)[3], 255);  // Program stays opaque
}

TEST(OverlayBlend, TransparentSourceAndOpacityAreNoOps) {
    auto f = opaqueFrame(2, 2, 7, 7, 7);
    Bitmap b; b.resize(2, 2);  // all (0,0,0,0)
    blendBitmap(f, b, 0, 0);
    EXPECT_EQ(px(f, 0, 0)[0], 7);
    b.bgra.assign(16, 255);
    blendBitmap(f, b, 0, 0, 0.0);
    EXPECT_EQ(px(f, 0, 0)[0], 7);
    blendBitmap(f, b, 0, 0, 0.5);
    EXPECT_NEAR(px(f, 0, 0)[0], 131, 2);
}

TEST(OverlayBlend, ClipsAndNeverOverruns) {
    auto f = opaqueFrame(4, 4, 0, 0, 0);
    Bitmap b; b.resize(8, 8); b.bgra.assign(b.bgra.size(), 255);
    blendBitmap(f, b, -6, -6);   // only 2x2 visible at top-left
    EXPECT_EQ(px(f, 1, 1)[0], 255);
    EXPECT_EQ(px(f, 2, 2)[0], 0);
    blendBitmap(f, b, 100, 100);  // fully outside
    blendBitmap(f, b, 3, 3);      // 1x1 visible
    EXPECT_EQ(px(f, 3, 3)[0], 255);
    hb::video::Frame empty;
    blendBitmap(empty, b, 0, 0);  // invalid frame: no crash
    Bitmap none;
    blendBitmap(f, none, 0, 0);
}

TEST(OverlayBlend, BitmapOverBitmapKeepsStraightAlpha) {
    Bitmap dst; dst.resize(1, 1);
    Bitmap src; src.resize(1, 1);
    src.bgra = {0, 0, 255, 128};  // red at 50%
    blendBitmap(dst, src, 0, 0);
    EXPECT_EQ(dst.bgra[3], 128);  // alpha accumulates
    EXPECT_EQ(dst.bgra[2], 255);  // colour stays the pure colour (straight alpha)
    blendBitmap(dst, src, 0, 0);
    EXPECT_NEAR(dst.bgra[3], 192, 1);  // 1 - 0.5*0.5 = 0.75
    EXPECT_EQ(dst.bgra[2], 255);
}

TEST(OverlayBlend, FillRect) {
    auto f = opaqueFrame(4, 4, 0, 0, 0);
    fillRect(f, 1, 1, 2, 2, 0xFF0000, 255);
    EXPECT_EQ(px(f, 1, 1)[2], 255); EXPECT_EQ(px(f, 0, 0)[2], 0);
    fillRect(f, -5, -5, 100, 100, 0x0000FF, 0);  // alpha 0: nothing
    EXPECT_EQ(px(f, 3, 3)[0], 0);
}

// ---------------- text ----------------
TEST(OverlayText, Utf8Helpers) {
    EXPECT_EQ(utf8SeqLen("a", 0), 1u);
    EXPECT_EQ(utf8SeqLen("\xC3\xA7", 0), 2u);
    EXPECT_EQ(utf8SeqLen("\xE2\x82\xAC", 0), 3u);
    EXPECT_EQ(utf8SeqLen("\xF0\x9F\x98\x80", 0), 4u);
    EXPECT_EQ(utf8SeqLen("\xC3", 0), 1u);  // truncated sequence still makes progress
    EXPECT_EQ(utf8Decode("\xC3\xA7", 0), 0xE7u);
    EXPECT_EQ(utf8Decode("\xF0\x9F\x98\x80", 0), 0x1F600u);
    EXPECT_EQ(utf8Decode("\xFF", 0), 0xFFFDu);
    EXPECT_EQ(sanitizeUtf8("a\x01" "b\tc\xFF" "d", 100), std::string("ab c\xEF\xBF\xBD" "d"));
    EXPECT_EQ(sanitizeUtf8("abcdef", 3), "abc");
    EXPECT_EQ(sanitizeUtf8("\xC3\xA7\xC3\xA7", 3), "\xC3\xA7");  // never cuts a sequence in half
}

TEST(OverlayText, MeasureAndWrap) {
    MaskTextRasterizer r(makeBuiltinGlyphSource());
    TextStyle st; st.px = 8;  // scale 1: 6 px per character, 8 px line
    TextMetrics m;
    ASSERT_TRUE(r.measure("ABCD", st, 0, m));
    EXPECT_EQ(m.width, 24); EXPECT_EQ(m.height, 8); EXPECT_EQ(m.lines, 1);
    auto lines = r.wrap("ONE TWO THREE", st, 6 * 7);  // 7 chars per line
    ASSERT_EQ(lines.size(), 2u);
    EXPECT_EQ(lines[0], "ONE TWO"); EXPECT_EQ(lines[1], "THREE");
    lines = r.wrap("AB\nCD", st, 0);
    ASSERT_EQ(lines.size(), 2u);
    lines = r.wrap("SUPERCALIFRAGILISTIC", st, 6 * 8);  // over-long word is split
    ASSERT_GE(lines.size(), 3u);
    for (auto& l : lines) EXPECT_LE(l.size(), 8u);
    st.maxLines = 2; st.keepTail = true;
    lines = r.wrap("A B C D E F G H", st, 6 * 3);
    ASSERT_EQ(lines.size(), 2u);
    EXPECT_EQ(lines.back(), "G H");  // tail kept
    st.keepTail = false;
    lines = r.wrap("A B C D E F G H", st, 6 * 3);
    EXPECT_EQ(lines.front(), "A B");
    EXPECT_FALSE(r.measure("", st, 0, m));
}

TEST(OverlayText, RasterizeColorOutlineShadow) {
    MaskTextRasterizer r(makeBuiltinGlyphSource());
    TextStyle st; st.px = 8; st.color = 0xFF0000;
    Bitmap b;
    ASSERT_TRUE(r.rasterize("I", st, 0, b));
    EXPECT_EQ(b.w, 6); EXPECT_EQ(b.h, 8);
    int opaque = 0; bool red = true;
    for (std::size_t i = 0; i < b.bgra.size(); i += 4)
        if (b.bgra[i + 3] == 255) { ++opaque; red = red && b.bgra[i + 2] == 255 && b.bgra[i] == 0; }
    EXPECT_GT(opaque, 5);
    EXPECT_TRUE(red);
    EXPECT_EQ(b.bgra[3], 0);  // corner pixel is transparent

    TextStyle fancy = st; fancy.outlinePx = 1; fancy.outlineColor = 0x00FF00; fancy.shadowPx = 2; fancy.shadowAlpha = 200;
    Bitmap f;
    ASSERT_TRUE(r.rasterize("I", fancy, 0, f));
    EXPECT_EQ(f.w, b.w + 6); EXPECT_EQ(f.h, b.h + 6);  // margins = outline + shadow on each side
    int green = 0, dark = 0;
    for (std::size_t i = 0; i < f.bgra.size(); i += 4) {
        if (f.bgra[i + 3] == 255 && f.bgra[i + 1] == 255 && f.bgra[i + 2] == 0) ++green;
        if (f.bgra[i + 3] > 0 && f.bgra[i + 3] < 255 && f.bgra[i + 2] == 0 && f.bgra[i + 1] == 0) ++dark;
    }
    EXPECT_GT(green, 0);  // outline ring
    EXPECT_GT(dark, 0);   // translucent shadow
}

TEST(OverlayText, AlignmentAndAccents) {
    MaskTextRasterizer r(makeBuiltinGlyphSource());
    TextStyle st; st.px = 8; st.align = Align::Right;
    Bitmap b;
    ASSERT_TRUE(r.rasterize("ABCDEF\nA", st, 0, b));
    // second line is right aligned: its first column (x=0) must be empty, last columns used
    bool leftEmpty = true, rightUsed = false;
    for (int y = 8; y < 16; ++y) {
        if (b.bgra[(y * b.w + 0) * 4 + 3]) leftEmpty = false;
        for (int x = b.w - 6; x < b.w; ++x) if (b.bgra[(y * b.w + x) * 4 + 3]) rightUsed = true;
    }
    EXPECT_TRUE(leftEmpty);
    EXPECT_TRUE(rightUsed);
    ASSERT_TRUE(r.rasterize("Ol\xC3\xA1 \xF0\x9F\x98\x80", st, 0, b));  // accents / emoji: placeholder glyphs, no crash
    EXPECT_EQ(b.w, 5 * 6);
}

TEST(OverlayText, HugeOrDegenerateInputIsRejected) {
    MaskTextRasterizer r(makeBuiltinGlyphSource());
    TextStyle st; st.px = 4000;
    Bitmap b;
    EXPECT_FALSE(r.rasterize("WIDE TEXT", st, 0, b));
    EXPECT_TRUE(b.empty());
}
