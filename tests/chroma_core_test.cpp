#include <gtest/gtest.h>

#include "chroma/chroma_key.hpp"
#include "chroma/compositor.hpp"
#include "chroma/mask_ops.hpp"
#include "chroma/types.hpp"
#include "chroma_test_util.hpp"

using namespace hb::chroma;
using hbt::px;
using hbt::setPx;
using hbt::solid;

TEST(ChromaTypes, RoundTripAndClamp) {
    Settings s;
    s.mode = Mode::Ai; s.tolerance = 70; s.softness = 10; s.spill = 5; s.quality = Quality::High;
    s.repl = ReplKind::Image; s.color = 0x12ab34; s.blur = 90; s.image = "C:\\a;b=c%d|e.png";
    const Settings t = parseSettings(serializeSettings(s));
    EXPECT_EQ(s, t);
    EXPECT_EQ(serializeSettings(s).find('\t'), std::string::npos);
    EXPECT_EQ(serializeSettings(s).find('\n'), std::string::npos);
}

TEST(ChromaTypes, HostileInputNeverThrowsAndClamps) {
    Settings s = parseSettings("mode=zzz;tol=abc;soft=-5;color=#12;img=%00%01x;;=;garbage;q=low");
    EXPECT_EQ(s.mode, Mode::Off);
    EXPECT_EQ(s.tolerance, 45);       // invalid value ignored
    EXPECT_EQ(s.softness, 0);         // clamped
    EXPECT_EQ(s.quality, Quality::Low);
    EXPECT_EQ(s.image, "x");          // control bytes never decoded into the path
    Settings c;
    EXPECT_TRUE(setField(c, "tol", "999"));
    EXPECT_EQ(c.tolerance, 100);
    EXPECT_FALSE(setField(c, "tol", "abc"));
    EXPECT_FALSE(setField(c, "nope", "1"));
    EXPECT_NO_THROW(parseSettings(std::string(100000, ';')));
    EXPECT_NO_THROW(parseSettings(""));
}

TEST(ChromaKey, GreenScreenKeysOutAndSkinStays) {
    auto f = solid(8, 4, 70, 190, 40);          // BGR of a typical screen
    setPx(f, 2, 1, 120, 150, 210);              // skin-ish
    Mask m;
    KeyParams kp;
    computeKeyMask(f, kp, m);
    ASSERT_TRUE(m.valid());
    EXPECT_EQ(m.data[0], 0);
    EXPECT_EQ(m.data[static_cast<std::size_t>(1) * 8 + 2], 255);
}

TEST(ChromaKey, BlueModeAndShadowsStillKey) {
    auto f = solid(4, 4, 200, 70, 40);          // blue screen
    setPx(f, 1, 1, 100, 35, 20);                // darker (shadowed) blue: same chroma, lower luma
    Mask m;
    KeyParams kp; kp.mode = Mode::Blue;
    computeKeyMask(f, kp, m);
    EXPECT_EQ(m.data[0], 0);
    EXPECT_LE(m.data[static_cast<std::size_t>(1) * 4 + 1], 10);
    kp.mode = Mode::Green;   // a green key must NOT remove a blue screen
    computeKeyMask(f, kp, m);
    EXPECT_EQ(m.data[0], 255);
}

TEST(ChromaKey, ToleranceMonotonic) {
    auto f = solid(1, 1, 90, 170, 80);   // a bit off the reference screen colour
    Mask a, b;
    KeyParams kp; kp.tolerance = 5; kp.softness = 0;
    computeKeyMask(f, kp, a);
    kp.tolerance = 80;
    computeKeyMask(f, kp, b);
    EXPECT_GE(a.data[0], b.data[0]);
    EXPECT_EQ(b.data[0], 0);
}

TEST(ChromaKey, DegenerateFramesAreSafe) {
    hb::video::Frame empty;
    Mask m;
    computeKeyMask(empty, KeyParams{}, m);
    EXPECT_FALSE(m.valid());
    hb::video::Frame shortBuf = solid(4, 4, 0, 0, 0);
    shortBuf.data.resize(10);
    computeKeyMask(shortBuf, KeyParams{}, m);
    EXPECT_FALSE(m.valid());
    despill(shortBuf, Spill::Green, 100);   // must not crash
}

TEST(ChromaKey, DespillClampsGreen) {
    auto f = solid(1, 1, 100, 200, 120);   // g above max(r,b)
    despill(f, Spill::Green, 100);
    EXPECT_EQ(px(f, 0, 0)[1], 120);
    auto g = solid(1, 1, 100, 200, 120);
    despill(g, Spill::Green, 50);
    EXPECT_EQ(px(g, 0, 0)[1], 160);
    auto h = solid(1, 1, 100, 100, 120);   // no spill: untouched
    despill(h, Spill::Green, 100);
    EXPECT_EQ(px(h, 0, 0)[1], 100);
}

TEST(ChromaMask, EmaSmoothsAndResets) {
    Mask acc, in;
    acc.resize(2, 2, 0);
    in.resize(2, 2, 255);
    emaBlend(acc, in, 128);
    EXPECT_EQ(acc.data[0], 128);
    emaBlend(acc, in, 128);
    EXPECT_EQ(acc.data[0], 192);
    Mask other; other.resize(3, 3, 7);
    emaBlend(acc, other, 128);   // size change: replaced
    EXPECT_EQ(acc.width, 3);
    EXPECT_EQ(acc.data[0], 7);
    Mask none;
    emaBlend(none, in, 100);
    EXPECT_EQ(none.width, 2);
}

TEST(ChromaMask, ErodeDilateBlurLevels) {
    Mask m; m.resize(5, 5, 0);
    m.data[12] = 255;                       // single pixel
    dilate(m, 1);
    EXPECT_EQ(m.data[7], 255);              // neighbour grew
    EXPECT_EQ(m.data[0], 0);
    erode(m, 1);
    EXPECT_EQ(m.data[12], 255);
    EXPECT_EQ(m.data[7], 0);
    Mask b; b.resize(5, 1, 0); b.data[2] = 255;
    boxBlur(b, 1);
    EXPECT_NEAR(b.data[2], 85, 1);
    EXPECT_NEAR(b.data[1], 85, 1);
    Mask l; l.resize(3, 1, 0); l.data[0] = 10; l.data[1] = 128; l.data[2] = 250;
    applyLevels(l, 100, 160);
    EXPECT_EQ(l.data[0], 0);
    EXPECT_GT(l.data[1], 0);
    EXPECT_LT(l.data[1], 255);
    EXPECT_EQ(l.data[2], 255);
    Mask e;
    erode(e, 3); dilate(e, 3); boxBlur(e, 3); applyLevels(e, 1, 2);   // invalid masks: no crash
}

TEST(ChromaMask, UpscaleBilinear) {
    Mask m; m.resize(2, 1); m.data[0] = 0; m.data[1] = 255;
    Mask o;
    upscaleBilinear(m, o, 8, 2);
    ASSERT_EQ(o.width, 8);
    EXPECT_EQ(o.data[0], 0);
    EXPECT_EQ(o.data[7], 255);
    for (int x = 1; x < 8; ++x) EXPECT_GE(o.data[static_cast<std::size_t>(x)], o.data[static_cast<std::size_t>(x - 1)]);
    Mask c; c.resize(3, 3, 77);
    upscaleBilinear(c, o, 10, 7);
    for (auto v : o.data) EXPECT_EQ(v, 77);
    upscaleBilinear(Mask{}, o, 4, 4);   // invalid source: zeros, no crash
    EXPECT_EQ(o.data[0], 0);
}

TEST(ChromaComposite, OpaqueColorReplacement) {
    auto f = solid(4, 1, 10, 20, 30);
    Mask m; m.resize(4, 1); m.data = {255, 0, 128, 0};
    Background bg; bg.kind = ReplKind::Color; bg.color = 0xFF0000;   // red
    compositeMatte(f, m, bg);
    EXPECT_EQ(px(f, 0, 0)[0], 10);              // foreground intact
    EXPECT_EQ(px(f, 1, 0)[2], 255);             // replaced
    EXPECT_EQ(px(f, 1, 0)[0], 0);
    EXPECT_NEAR(px(f, 2, 0)[2], 143, 2);        // half-way blend
    for (int x = 0; x < 4; ++x) EXPECT_EQ(px(f, x, 0)[3], 255);
}

TEST(ChromaComposite, TransparentIsStraightAlpha) {
    auto f = solid(2, 1, 10, 20, 30);
    Mask m; m.resize(2, 1); m.data = {255, 100};
    Background bg; bg.kind = ReplKind::Transparent;
    compositeMatte(f, m, bg);
    EXPECT_EQ(px(f, 1, 0)[3], 100);
    EXPECT_EQ(px(f, 1, 0)[0], 10);   // colour NOT premultiplied
    EXPECT_EQ(px(f, 1, 0)[2], 30);
}

TEST(ChromaComposite, ImageBackgroundAndMaskUpscale) {
    auto f = solid(4, 2, 1, 2, 3);
    auto img = solid(4, 2, 200, 100, 50);
    Mask m; m.resize(2, 1); m.data = {255, 0};   // left person, right background (upscaled)
    Background bg; bg.kind = ReplKind::Image; bg.image = &img;
    compositeMatte(f, m, bg);
    EXPECT_EQ(px(f, 0, 0)[0], 1);
    EXPECT_EQ(px(f, 3, 1)[0], 200);
    auto small = solid(2, 2, 9, 9, 9);   // wrong-sized image is ignored (colour fallback)
    auto g = solid(4, 2, 1, 2, 3);
    bg.image = &small; bg.color = 0x00ff00;
    compositeMatte(g, m, bg);
    EXPECT_EQ(px(g, 3, 1)[1], 255);
}

TEST(ChromaComposite, CoverScaleAndBlur) {
    auto img = solid(8, 4, 10, 20, 30);
    hb::video::Frame out;
    coverScale(img, out, 4, 4);   // 2:1 into 1:1 -> centre crop
    EXPECT_EQ(out.width, 4);
    EXPECT_EQ(px(out, 0, 0)[2], 30);
    hb::video::Frame bad;
    coverScale(bad, out, 4, 4);   // invalid source: black opaque
    EXPECT_EQ(px(out, 0, 0)[3], 255);
    auto f = solid(64, 64, 0, 0, 0);
    for (int y = 0; y < 64; ++y)
        for (int x = 32; x < 64; ++x) setPx(f, x, y, 255, 255, 255);
    hb::video::Frame bl;
    makeBlurBackground(f, 60, bl);
    EXPECT_EQ(bl.width, 64);
    const int edge = px(bl, 32, 32)[0];
    EXPECT_GT(edge, 20);
    EXPECT_LT(edge, 235);   // the hard edge got soft
    makeBlurBackground(f, 0, bl);
    EXPECT_EQ(px(bl, 0, 0)[0], 0);
}
