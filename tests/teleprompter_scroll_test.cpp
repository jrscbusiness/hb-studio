#include <gtest/gtest.h>

#include <cmath>

#include "teleprompter/scroll.hpp"

using namespace hb::teleprompter;

namespace {
struct Fx {
    ManualClock clock;
    ScrollState s{&clock};
    Fx() {
        s.setScriptWords(300);          // 300 words
        s.setLayout(6000, 1000);        // 6000 px tall
        s.setWpm(150);                  // 150 wpm -> 120 s total
        s.setCountdown(0);
    }
};
}  // namespace

TEST(TpScroll, WpmPxConversionRoundTrip) {
    // 300 words, 6000 px, 150 wpm: 150/60 = 2.5 words/s; 20 px/word -> 50 px/s
    EXPECT_NEAR(wpmToPxPerSec(150, 300, 6000), 50.0, 1e-9);
    EXPECT_NEAR(pxPerSecToWpm(50, 300, 6000), 150.0, 1e-9);
    EXPECT_EQ(wpmToPxPerSec(150, 0, 6000), 0.0);
    EXPECT_EQ(wpmToPxPerSec(150, 300, 0), 0.0);
    EXPECT_EQ(wpmToPxPerSec(-5, 300, 6000), 0.0);
    EXPECT_EQ(pxPerSecToWpm(NAN, 300, 6000), 0.0);
}

TEST(TpScroll, Clamps) {
    EXPECT_EQ(clampWpm(1), kMinWpm);
    EXPECT_EQ(clampWpm(9999), kMaxWpm);
    EXPECT_EQ(clampWpm(NAN), kMinWpm);
    EXPECT_EQ(clampPxPerSec(0), kMinPxPerSec);
}

TEST(TpScroll, PlaysAtWpmRateAndEstimatesTime) {
    Fx f;
    auto a = f.s.snapshot();
    EXPECT_EQ(a.state, RunState::Stopped);
    EXPECT_NEAR(a.totalSec, 120.0, 1e-6);
    EXPECT_NEAR(a.pxPerSec, 50.0, 1e-6);
    f.s.play();
    f.clock.advance(30);
    a = f.s.snapshot();
    EXPECT_EQ(a.state, RunState::Playing);
    EXPECT_NEAR(a.pos, 0.25, 1e-9);
    EXPECT_NEAR(a.remainingSec, 90.0, 1e-6);
    EXPECT_NEAR(a.rate, 1.0 / 120.0, 1e-12);
}

TEST(TpScroll, PauseHoldsPositionAndResumes) {
    Fx f;
    f.s.play();
    f.clock.advance(60);
    f.s.pause();
    f.clock.advance(500);
    auto a = f.s.snapshot();
    EXPECT_EQ(a.state, RunState::Paused);
    EXPECT_NEAR(a.pos, 0.5, 1e-9);
    EXPECT_EQ(a.rate, 0.0);
    f.s.play();
    f.clock.advance(12);
    EXPECT_NEAR(f.s.snapshot().pos, 0.6, 1e-9);
}

TEST(TpScroll, FinishesAndReplayRestarts) {
    Fx f;
    f.s.play();
    f.clock.advance(1000);
    auto a = f.s.snapshot();
    EXPECT_EQ(a.state, RunState::Finished);
    EXPECT_EQ(a.pos, 1.0);
    EXPECT_EQ(a.remainingSec, 0.0);
    f.s.play();   // from Finished: back to the top
    f.clock.advance(12);
    a = f.s.snapshot();
    EXPECT_EQ(a.state, RunState::Playing);
    EXPECT_NEAR(a.pos, 0.1, 1e-9);
}

TEST(TpScroll, CountdownThenPlayWithoutDrift) {
    Fx f;
    f.s.setCountdown(3);
    f.s.play();
    auto a = f.s.snapshot();
    EXPECT_EQ(a.state, RunState::Countdown);
    EXPECT_NEAR(a.countdownLeft, 3.0, 1e-9);
    EXPECT_EQ(a.pos, 0.0);
    EXPECT_NEAR(a.remainingSec, 123.0, 1e-6);
    f.clock.advance(2);
    EXPECT_NEAR(f.s.snapshot().countdownLeft, 1.0, 1e-9);
    // Polled late (5 s after play): movement counts from the exact end of the countdown (t=3 s).
    f.clock.advance(3);
    a = f.s.snapshot();
    EXPECT_EQ(a.state, RunState::Playing);
    EXPECT_NEAR(a.pos, 2.0 / 120.0, 1e-9);
}

TEST(TpScroll, CountdownOnlyAtTheStartAndCancellable) {
    Fx f;
    f.s.setCountdown(3);
    f.s.seek(0.5);
    f.s.play();   // mid-script resume: no countdown
    EXPECT_EQ(f.s.snapshot().state, RunState::Playing);
    f.s.restart();
    f.s.play();
    EXPECT_EQ(f.s.snapshot().state, RunState::Countdown);
    f.s.pause();
    auto a = f.s.snapshot();
    EXPECT_EQ(a.state, RunState::Paused);
    EXPECT_EQ(a.pos, 0.0);
    f.clock.advance(10);
    EXPECT_EQ(f.s.snapshot().pos, 0.0);
}

TEST(TpScroll, SeekWhilePlayingReanchors) {
    Fx f;
    f.s.play();
    f.clock.advance(10);
    f.s.seek(0.5);
    f.clock.advance(12);
    EXPECT_NEAR(f.s.snapshot().pos, 0.6, 1e-9);
    f.s.seek(2.0);   // clamped: end of script
    EXPECT_EQ(f.s.snapshot().state, RunState::Finished);
    f.s.seek(NAN);   // ignored
    EXPECT_EQ(f.s.snapshot().pos, 1.0);
    f.s.seek(0.3);
    EXPECT_EQ(f.s.snapshot().state, RunState::Paused);
    f.s.seek(-1);
    EXPECT_EQ(f.s.snapshot().pos, 0.0);
}

TEST(TpScroll, SpeedChangeWhilePlayingKeepsPosition) {
    Fx f;
    f.s.play();
    f.clock.advance(60);   // pos 0.5
    f.s.setWpm(300);       // twice as fast from now on
    EXPECT_NEAR(f.s.snapshot().pos, 0.5, 1e-9);
    f.clock.advance(30);   // 30 s at 300 wpm = 0.5 of the script
    auto a = f.s.snapshot();
    EXPECT_EQ(a.state, RunState::Finished);
    EXPECT_EQ(a.pos, 1.0);
}

TEST(TpScroll, NudgeIsClampedAndUsesCurrentModeUnit) {
    Fx f;
    f.s.nudgeSpeed(+10);
    EXPECT_EQ(f.s.wpm(), 160);
    f.s.nudgeSpeed(-10000);
    EXPECT_EQ(f.s.wpm(), kMinWpm);
    f.s.setMode(SpeedMode::PxPerSec);
    f.s.setPxPerSec(100);
    f.s.nudgeSpeed(-30);
    EXPECT_EQ(f.s.pxPerSec(), 70);
}

TEST(TpScroll, PxModeRateDependsOnContentHeight) {
    Fx f;
    f.s.setMode(SpeedMode::PxPerSec);
    f.s.setPxPerSec(60);   // 6000 px at 60 px/s = 100 s
    auto a = f.s.snapshot();
    EXPECT_NEAR(a.totalSec, 100.0, 1e-6);
    EXPECT_NEAR(a.wpm, 180.0, 1e-6);
    f.s.play();
    f.clock.advance(50);
    EXPECT_NEAR(f.s.snapshot().pos, 0.5, 1e-9);
    f.s.setLayout(12000, 1000);   // text re-wrapped twice as tall: same px/s -> half the fraction speed
    f.clock.advance(50);
    EXPECT_NEAR(f.s.snapshot().pos, 0.75, 1e-9);
}

TEST(TpScroll, ModeSwitchPreservesEffectiveSpeed) {
    Fx f;   // 150 wpm = 50 px/s
    f.s.setMode(SpeedMode::PxPerSec);
    EXPECT_NEAR(f.s.pxPerSec(), 50.0, 1e-9);
    f.s.setMode(SpeedMode::Wpm);
    EXPECT_NEAR(f.s.wpm(), 150.0, 1e-9);
}

TEST(TpScroll, UnknownScriptOrLayoutIsInert) {
    ManualClock c;
    ScrollState s(&c);   // no words, no layout
    s.play();
    c.advance(100);
    auto a = s.snapshot();
    EXPECT_EQ(a.state, RunState::Playing);   // the default 3 s countdown elapsed; nothing to move
    EXPECT_EQ(a.rate, 0.0);
    EXPECT_EQ(a.totalSec, 0.0);
    s.setLayout(NAN, 100);   // rejected
    s.setLayout(-5, 100);
    EXPECT_EQ(s.snapshot().contentPx, 0.0);
}

TEST(TpScroll, RestartResetsEverything) {
    Fx f;
    f.s.play();
    f.clock.advance(30);
    f.s.restart();
    auto a = f.s.snapshot();
    EXPECT_EQ(a.state, RunState::Stopped);
    EXPECT_EQ(a.pos, 0.0);
}

TEST(TpScroll, ToggleAndNames) {
    Fx f;
    f.s.toggle();
    EXPECT_EQ(f.s.snapshot().state, RunState::Playing);
    f.s.toggle();
    EXPECT_EQ(f.s.snapshot().state, RunState::Paused);
    EXPECT_STREQ(runStateName(RunState::Countdown), "countdown");
    EXPECT_STREQ(speedModeName(SpeedMode::PxPerSec), "pxs");
}
