#include <gtest/gtest.h>

#include "overlay/timing.hpp"

using namespace hb::overlay;

TEST(OverlayTimer, CountdownRoundsUpAndStopsAtZero) {
    Timer t;
    t.configure(Timer::Mode::Down, 5000);
    EXPECT_EQ(t.text(0, "auto"), "00:05");
    t.start(1000);
    EXPECT_EQ(t.text(1000, "auto"), "00:05");
    EXPECT_EQ(t.text(1500, "auto"), "00:05");  // 4.5 s left shows 5 (rounds up)
    EXPECT_EQ(t.text(2001, "auto"), "00:04");
    EXPECT_FALSE(t.finished(5999));
    EXPECT_EQ(t.text(5999, "auto"), "00:01");
    EXPECT_TRUE(t.finished(6000));
    EXPECT_EQ(t.text(60000, "auto"), "00:00");
    EXPECT_FALSE(t.running(60000));
}

TEST(OverlayTimer, CountUpPauseResume) {
    Timer t;
    t.configure(Timer::Mode::Up, 0);
    t.start(0);
    EXPECT_EQ(t.text(61999, "auto"), "01:01");
    t.pause(70000);
    EXPECT_EQ(t.text(500000, "auto"), "01:10");  // frozen
    t.start(600000);
    EXPECT_EQ(t.text(610000, "auto"), "01:20");
    t.reset();
    EXPECT_EQ(t.text(610000, "auto"), "00:00");
    EXPECT_FALSE(t.running());
}

TEST(OverlayTimer, Formats) {
    EXPECT_EQ(Timer::format(59, "auto"), "00:59");
    EXPECT_EQ(Timer::format(3600, "auto"), "01:00:00");
    EXPECT_EQ(Timer::format(3725, "hh:mm:ss"), "01:02:05");
    EXPECT_EQ(Timer::format(5, "hh:mm:ss"), "00:00:05");
    EXPECT_EQ(Timer::format(3725, "mm:ss"), "62:05");
    EXPECT_EQ(Timer::format(-4, "auto"), "00:00");
}

TEST(OverlayTimer, AdjustAndRestartAfterFinish) {
    Timer t;
    t.configure(Timer::Mode::Down, 60000);
    t.start(0);
    t.adjust(10000, 30000);  // +30 s remaining: 50 + 30 = 80 -> clamped to the 60 s duration
    EXPECT_EQ(t.displaySeconds(10000), 60);
    t.adjust(10000, -45000);
    EXPECT_EQ(t.displaySeconds(10000), 15);
    t.start(10000);  // already running: no-op
    EXPECT_EQ(t.displaySeconds(15000), 10);
    t.pause(15000);
    t.adjust(15000, -100000);
    EXPECT_EQ(t.displaySeconds(15000), 0);
    EXPECT_TRUE(t.finished(15000));
    t.start(16000);  // finished: cannot start until reset
    EXPECT_FALSE(t.running());
    t.reset();
    t.start(20000);
    EXPECT_TRUE(t.running());
}

TEST(OverlayTimer, ZeroDurationCountdownNeverFinishes) {
    Timer t;
    t.configure(Timer::Mode::Down, 0);
    t.start(0);
    EXPECT_FALSE(t.finished(1000));
}

TEST(OverlayAnimator, FadeInOutAndReverseMidway) {
    Animator a;
    EXPECT_DOUBLE_EQ(a.progress(0), 0.0);
    EXPECT_FALSE(a.active(0));
    a.set(true, 1000, 400, 400);
    EXPECT_NEAR(a.progress(1200), 0.5, 1e-9);
    EXPECT_DOUBLE_EQ(a.progress(1400), 1.0);
    EXPECT_TRUE(a.active(5000));
    a.set(false, 5000, 400, 400);
    EXPECT_NEAR(a.progress(5200), 0.5, 1e-9);
    EXPECT_TRUE(a.active(5200));
    EXPECT_FALSE(a.active(5400));
    // reverse in the middle of a fade-out: continues from 0.5, not from 0 or 1
    a.set(true, 5200, 400, 400);
    EXPECT_NEAR(a.progress(5200), 0.5, 1e-9);
    EXPECT_NEAR(a.progress(5400), 0.75, 1e-9);
}

TEST(OverlayAnimator, ZeroDurationIsInstant) {
    Animator a;
    a.set(true, 10, 0, 0);
    EXPECT_DOUBLE_EQ(a.progress(10), 1.0);
    a.set(false, 20, 0, 0);
    EXPECT_DOUBLE_EQ(a.progress(20), 0.0);
    EXPECT_NEAR(Animator::ease(0.5), 0.5, 1e-9);
}
