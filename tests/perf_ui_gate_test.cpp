#include <gtest/gtest.h>

#include "perf/ui_frame_gate.hpp"
#include "video/frame_ops.hpp"

using namespace hb;
using namespace hb::perf;

namespace {
video::Frame solid(int w, int h, std::uint8_t v) {
    video::Frame f;
    video::prepareFrame(f, w, h);
    std::fill(f.data.begin(), f.data.end(), v);
    return f;
}
}  // namespace

TEST(UiFrameGate, UnchangedFramesAreSuppressedUntilKeepAlive) {
    UiFrameGate g(1.0);
    const auto f = solid(64, 36, 10);
    EXPECT_TRUE(g.shouldSend("pgm", f, 0.0));
    EXPECT_FALSE(g.shouldSend("pgm", f, 0.1));
    EXPECT_FALSE(g.shouldSend("pgm", f, 0.9));
    EXPECT_TRUE(g.shouldSend("pgm", f, 1.05));   // keep-alive: a reloaded page gets a picture within a second
    EXPECT_FALSE(g.shouldSend("pgm", f, 1.2));
    EXPECT_EQ(g.sent(), 2u);
    EXPECT_EQ(g.suppressed(), 3u);
}

TEST(UiFrameGate, AnyChangedPixelIsSent) {
    UiFrameGate g;
    auto f = solid(64, 36, 10);
    EXPECT_TRUE(g.shouldSend("pvw", f, 0.0));
    for (std::size_t i : {std::size_t{0}, std::size_t{3}, f.data.size() / 2, f.data.size() - 1}) {
        f.data[i] ^= 1;
        EXPECT_TRUE(g.shouldSend("pvw", f, 0.01)) << i;
        EXPECT_FALSE(g.shouldSend("pvw", f, 0.02)) << i;
    }
}

TEST(UiFrameGate, IdsAreIndependentAndResetForcesResend) {
    UiFrameGate g;
    const auto f = solid(32, 18, 7);
    EXPECT_TRUE(g.shouldSend("s0", f, 0));
    EXPECT_TRUE(g.shouldSend("s1", f, 0));   // same pixels, other tile: still its own first frame
    EXPECT_FALSE(g.shouldSend("s0", f, 0.1));
    g.reset();
    EXPECT_TRUE(g.shouldSend("s0", f, 0.2));
}

TEST(UiFrameGate, HashDependsOnSizeAndHandlesOddAndInvalidFrames) {
    EXPECT_NE(hashFrame(solid(32, 18, 0)), hashFrame(solid(18, 32, 0)));
    EXPECT_NE(hashFrame(solid(33, 7, 5)), hashFrame(solid(33, 7, 6)));
    video::Frame bad;
    (void)hashFrame(bad);
    video::Frame shortData = solid(16, 16, 1);
    shortData.data.resize(10);
    (void)hashFrame(shortData);
    SUCCEED();
}

TEST(UiFramePolicy, HotTabsKeepFullRateColdTabsThrottle) {
    UiFramePolicy p;
    EXPECT_TRUE(p.hot());
    for (std::uint64_t t = 0; t < 16; ++t) EXPECT_TRUE(p.monitorDue(t));
    p.setActiveTab("cfg");
    EXPECT_FALSE(p.hot());
    int monitors = 0;
    for (std::uint64_t t = 0; t < 16; ++t) monitors += p.monitorDue(t);
    EXPECT_EQ(monitors, 4);   // every 4th tick (~3.75 fps)
    // Each of 16 tiles is sent exactly once per 8 ticks when the feeder's parity alternation is applied.
    for (int i = 0; i < 16; ++i) {
        int sent = 0;
        for (std::uint64_t t = 0; t < 8; ++t)
            if (static_cast<std::uint64_t>(i % 2) == t % 2 && p.tileDue(t, i)) ++sent;
        EXPECT_EQ(sent, 1) << "tile " << i;
    }
    p.setActiveTab("gfx");
    EXPECT_TRUE(p.hot());
    p.setActiveTab("");           // ignored
    p.setActiveTab(std::string(40, 'x'));   // ignored
    EXPECT_EQ(p.activeTab(), "gfx");
}
