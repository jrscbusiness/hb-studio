#include <gtest/gtest.h>

#include <cmath>
#include <cstdlib>

#include "multitrack/track_timeline.hpp"

using namespace hb::multitrack;

TEST(MtMath, RoundTripExactAndLong) {
    // 29.97 fps over 48 h must not drift: slot -> ns -> slot is the identity.
    for (std::uint64_t slot : {0ull, 1ull, 29ull, 30ull, 17982ull, 5178816ull /* 48 h at 29.97 */}) {
        const Ns ns = unitsToNs(slot, 30000, 1001);
        EXPECT_EQ(nsToUnits(ns, 30000, 1001), static_cast<std::int64_t>(slot));
    }
    EXPECT_EQ(unitsToNs(30000, 30000, 1001), 1001000000000);   // 30000 frames at 29.97 fps = 1001 s
    EXPECT_EQ(unitsToNs(48000, 48000, 1), kNsPerSec);
    EXPECT_EQ(nsToUnits(kNsPerSec, 48000, 1), 48000);
    EXPECT_EQ(nsToUnits(-kNsPerSec / 2, 48000, 1), -24000);
    EXPECT_EQ(nsToUnits(0, 30, 1), 0);
    // rounding to nearest
    EXPECT_EQ(nsToUnits(16666666, 60, 1), 1);
    EXPECT_EQ(nsToUnits(8000000, 60, 1), 0);
    EXPECT_EQ(nsToUnits(9000000, 60, 1), 1);
}

TEST(MtVideo, SteadyCameraHasNoRepeatsOrDrops) {
    VideoTimeline t({30, 1});
    for (int i = 0; i < 300; ++i) {
        const auto p = t.onFrame(unitsToNs(5 + i, 30, 1) + (i % 7) * kNsPerMs);   // jitter < half a frame
        ASSERT_FALSE(p.drop);
        EXPECT_EQ(p.slot, 5u + i);
        EXPECT_EQ(p.repeatCount, 0u);
    }
    EXPECT_EQ(t.firstSlot(), 5u);
    EXPECT_EQ(t.written(), 300u);
    EXPECT_EQ(t.repeated(), 0u);
    EXPECT_EQ(t.dropped(), 0u);
}

TEST(MtVideo, StallIsFilledByRepeatsThenJumped) {
    VideoTimeline t({30, 1});
    t.onFrame(0);
    t.onFrame(unitsToNs(1, 30, 1));
    auto p = t.onFrame(unitsToNs(5, 30, 1));      // 3 missing slots: 2,3,4
    EXPECT_EQ(p.slot, 5u);
    EXPECT_EQ(p.repeatFrom, 2u);
    EXPECT_EQ(p.repeatCount, 3u);
    EXPECT_EQ(p.jumpedSlots, 0u);
    // 10 s stall with the default cap of 2 s (60 slots)
    p = t.onFrame(unitsToNs(6 + 300, 30, 1));
    EXPECT_EQ(p.repeatFrom, 6u);
    EXPECT_EQ(p.repeatCount, 60u);
    EXPECT_EQ(p.jumpedSlots, 300u - 60u);
    EXPECT_EQ(t.jumped(), 240u);
    EXPECT_EQ(t.worstGapSlots(), 300u);
}

TEST(MtVideo, FastCameraFramesAreDropped) {
    VideoTimeline t({30, 1});
    int dropped = 0, kept = 0;
    for (int i = 0; i < 120; ++i) {            // a 60 fps camera feeding a 30 fps track
        const auto p = t.onFrame(unitsToNs(i, 60, 1));
        (p.drop ? dropped : kept)++;
    }
    EXPECT_EQ(kept, 60);
    EXPECT_EQ(dropped, 60);
    EXPECT_EQ(t.dropped(), 60u);
}

TEST(MtVideo, NtscDoesNotDriftOverADay) {
    VideoTimeline t({30000, 1001});
    std::uint64_t expected = 0;
    for (std::uint64_t i = 0; i < 2592000ull; i += 1000) {   // 24 h at 29.97, sampled every 1000th frame
        const auto p = t.onFrame(unitsToNs(i, 30000, 1001));
        ASSERT_FALSE(p.drop);
        expected = i;
        ASSERT_EQ(p.slot, i);
    }
    EXPECT_EQ(t.slotNs(expected), unitsToNs(expected, 30000, 1001));
}

TEST(MtVideo, NegativeFirstStampClampsToZero) {
    VideoTimeline t({25, 1});
    EXPECT_EQ(t.onFrame(-3 * kNsPerMs).slot, 0u);
}

TEST(MtAudio, FirstBlockIsAlignedSampleAccurately) {
    AudioTimeline a({48000});
    // First block starts 15 ms after the session start: 720 samples of leading silence.
    auto p = a.onBlock(15 * kNsPerMs, 480);
    EXPECT_EQ(p.silenceBefore, 720u);
    EXPECT_EQ(p.skipFrames, 0u);
    EXPECT_EQ(a.position(), 1200u);
    EXPECT_EQ(a.leadingSilence(), 720u);
}

TEST(MtAudio, BlockBeforeSessionStartIsTrimmed) {
    AudioTimeline a({48000});
    auto p = a.onBlock(-5 * kNsPerMs, 480);         // 240 samples predate the session
    EXPECT_EQ(p.skipFrames, 240u);
    EXPECT_EQ(a.position(), 240u);
    AudioTimeline b({48000});
    p = b.onBlock(-50 * kNsPerMs, 480);             // the whole block predates it
    EXPECT_EQ(p.skipFrames, 480u);
    EXPECT_EQ(b.position(), 0u);
    p = b.onBlock(-40 * kNsPerMs, 480);             // still first: trims 1920 -> skip everything again
    EXPECT_EQ(p.skipFrames, 480u);
    p = b.onBlock(0, 480);
    EXPECT_EQ(p.skipFrames, 0u);
    EXPECT_EQ(p.silenceBefore, 0u);
    EXPECT_EQ(b.position(), 480u);
}

TEST(MtAudio, JitterWithinToleranceIsIgnored) {
    AudioTimeline a({48000});
    std::uint64_t pos = 0;
    a.onBlock(0, 480);
    pos = 480;
    for (int i = 1; i < 1000; ++i) {
        const Ns jitter = (i * 37 % 9) * 100 * 1000;   // up to 0.8 ms late, never early
        const auto p = a.onBlock(unitsToNs(pos, 48000, 1) + jitter, 480);
        ASSERT_EQ(p.silenceBefore + p.repeatLast + p.skipFrames, 0u);
        pos += 480;
    }
    EXPECT_EQ(a.position(), pos);
    EXPECT_EQ(a.netCorrectionFrames(), 0);
}

TEST(MtAudio, RealGapIsFilledWithSilenceAtOnce) {
    AudioTimeline a({48000});
    a.onBlock(0, 480);
    a.onBlock(unitsToNs(480, 48000, 1), 480);
    const Ns late = unitsToNs(960, 48000, 1) + 300 * kNsPerMs;   // 300 ms hole (engine xrun)
    const auto p = a.onBlock(late, 480);
    EXPECT_EQ(p.silenceBefore, 14400u);
    EXPECT_EQ(a.insertedSilence(), 14400u);
    EXPECT_EQ(a.position(), 960u + 14400u + 480u);
    // the block now lands exactly on the clock again
    EXPECT_EQ(unitsToNs(a.position(), 48000, 1), late + 10 * kNsPerMs);
}

TEST(MtAudio, OverlapIsSkipped) {
    AudioTimeline a({48000});
    a.onBlock(0, 480);
    const auto p = a.onBlock(unitsToNs(480, 48000, 1) - 250 * kNsPerMs, 480);   // 250 ms early (burst)
    EXPECT_EQ(p.skipFrames, 480u);   // can only drop the block it was given
    EXPECT_EQ(a.position(), 480u);
}

TEST(MtAudio, ExternalGapAdvancesPosition) {
    AudioTimeline a({48000});
    a.onBlock(0, 480);
    a.addExternalGap(4800);
    EXPECT_EQ(a.position(), 5280u);
    const auto p = a.onBlock(unitsToNs(5280, 48000, 1), 480);
    EXPECT_EQ(p.silenceBefore + p.repeatLast + p.skipFrames, 0u);
}

namespace {
// Simulates a source whose clock is `ppm` slower (+) or faster (-) than the session clock.
struct DriftRun {
    std::uint64_t outFrames = 0;
    Ns lastOffset = 0;
    double ppm = 0;
    std::int64_t net = 0;
};
DriftRun runDrift(double ppm, int seconds) {
    AudioTimeline a({48000});
    const double scale = 1.0 + ppm * 1e-6;     // wall time per source block, relative
    DriftRun r;
    const int blocks = seconds * 100;
    std::uint64_t pos = 0;
    for (int k = 0; k < blocks; ++k) {
        const Ns start = static_cast<Ns>(static_cast<double>(k) * 10e6 * scale);
        const auto p = a.onBlock(start, 480);
        pos += p.silenceBefore + p.repeatLast + (480 - p.skipFrames);
    }
    r.outFrames = pos;
    r.lastOffset = a.lastOffsetNs();
    r.net = a.netCorrectionFrames();
    r.ppm = a.driftPpm(static_cast<Ns>(blocks) * 10 * kNsPerMs);
    EXPECT_EQ(pos, a.position());
    return r;
}
}  // namespace

TEST(MtAudio, SlowSourceClockIsAbsorbedGradually) {
    const auto r = runDrift(200.0, 600);      // 10 minutes, 200 ppm slow
    EXPECT_LT(std::llabs(r.lastOffset), 6 * kNsPerMs) << "residual offset after 10 min";
    EXPECT_NEAR(r.ppm, 200.0, 25.0);
    // The file holds the session duration within a few ms: 600 s * 48000 = 28.8 M frames
    // The file ends where the session clock says the last block ends (wall time = 600 s * 1.0002).
    EXPECT_NEAR(static_cast<double>(r.outFrames), 59999 * 480.0 * 1.0002 + 480, 48000.0 * 0.006);
}

TEST(MtAudio, FastSourceClockIsAbsorbedGradually) {
    const auto r = runDrift(-150.0, 600);
    EXPECT_LT(std::llabs(r.lastOffset), 6 * kNsPerMs);
    EXPECT_NEAR(r.ppm, -150.0, 25.0);
    EXPECT_NEAR(static_cast<double>(r.outFrames), 59999 * 480.0 * 0.99985 + 480, 48000.0 * 0.006);
}

TEST(MtAudio, PerfectClockNeverCorrects) {
    const auto r = runDrift(0.0, 300);
    EXPECT_EQ(r.net, 0);
    EXPECT_EQ(r.outFrames, 300u * 48000u);
}
