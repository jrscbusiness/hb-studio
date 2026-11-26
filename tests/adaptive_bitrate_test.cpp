#include <gtest/gtest.h>

#include "streaming/adaptive_bitrate.hpp"

using namespace hb::streaming;

namespace {
AbrConfig cfg(int target = 6000) {
    AbrConfig c;
    c.targetKbps = target;
    return c;
}
AbrSample sample(int depth, std::uint64_t dropped = 0, int cap = 90) { return {depth, cap, dropped}; }
}  // namespace

TEST(AdaptiveBitrate, StartsAtTargetAndHealthyNeverChanges) {
    AdaptiveBitrate a(cfg());
    EXPECT_EQ(a.currentKbps(), 6000);
    EXPECT_EQ(a.minKbps(), 2400);   // 40%
    for (int t = 0; t < 200; ++t) EXPECT_EQ(a.update(t * 1000, sample(1)), 0);
    EXPECT_EQ(a.currentKbps(), 6000);
}

TEST(AdaptiveBitrate, SteppsDownOnlyAfterSustainedStress) {
    AdaptiveBitrate a(cfg());
    EXPECT_EQ(a.update(0, sample(60)), 0);       // 60/90 > 50%: stressed tick 1
    EXPECT_EQ(a.update(1000, sample(60)), 0);    // tick 2
    EXPECT_EQ(a.update(2000, sample(60)), 5100); // tick 3 -> x0.85
    EXPECT_EQ(a.currentKbps(), 5100);
}

TEST(AdaptiveBitrate, SingleSpikeDoesNotReduce) {
    AdaptiveBitrate a(cfg());
    EXPECT_EQ(a.update(0, sample(80)), 0);
    EXPECT_EQ(a.update(1000, sample(2)), 0);     // recovered: stress streak broken
    EXPECT_EQ(a.update(2000, sample(80)), 0);
    EXPECT_EQ(a.update(3000, sample(2)), 0);
    EXPECT_EQ(a.currentKbps(), 6000);
}

TEST(AdaptiveBitrate, DropsAloneCountAsStress) {
    AdaptiveBitrate a(cfg());
    EXPECT_EQ(a.update(0, sample(0, 0)), 0);            // baseline
    EXPECT_EQ(a.update(1000, sample(0, 5)), 0);
    EXPECT_EQ(a.update(2000, sample(0, 9)), 0);
    EXPECT_EQ(a.update(3000, sample(0, 14)), 5100);
}

TEST(AdaptiveBitrate, GrowingQueueCountsAsStressBeforeItIsFull) {
    AdaptiveBitrate a(cfg());
    EXPECT_EQ(a.update(0, sample(25)), 0);   // 25/90 = 28% >= 25% and growing
    EXPECT_EQ(a.update(1000, sample(30)), 0);
    EXPECT_EQ(a.update(2000, sample(35)), 5100);
}

TEST(AdaptiveBitrate, CooldownBetweenReductionsAndFloorAtFortyPercent) {
    AdaptiveBitrate a(cfg());
    std::int64_t t = 0;
    int last = 6000;
    int reductions = 0;
    for (int i = 0; i < 100; ++i, t += 1000) {
        const int r = a.update(t, sample(80));
        if (r) {
            EXPECT_LT(r, last);
            last = r;
            ++reductions;
        }
    }
    EXPECT_EQ(a.currentKbps(), 2400);   // clamped at the floor, never below
    EXPECT_GE(reductions, 4);
    // cooldown: two consecutive reductions are at least downCooldownMs apart
    AdaptiveBitrate b(cfg());
    std::int64_t lastChange = -1;
    for (std::int64_t tt = 0; tt < 60000; tt += 500) {
        if (b.update(tt, sample(80))) {
            if (lastChange >= 0) EXPECT_GE(tt - lastChange, 5000);
            lastChange = tt;
        }
    }
}

TEST(AdaptiveBitrate, RecoversSlowlyAfterAHealthyPeriodAndNeverExceedsTarget) {
    AdaptiveBitrate a(cfg());
    std::int64_t t = 0;
    for (int i = 0; i < 4; ++i, t += 1000) a.update(t, sample(80));
    const int reduced = a.currentKbps();
    ASSERT_LT(reduced, 6000);

    // healthy, but not yet for healthyMsToUp: no change
    for (int i = 0; i < 15; ++i, t += 1000) EXPECT_EQ(a.update(t, sample(1)), 0);
    // after 20 s healthy it steps up by 5% of target (300 kbps)
    int up = 0;
    for (int i = 0; i < 10 && !up; ++i, t += 1000) up = a.update(t, sample(1));
    EXPECT_EQ(up, reduced + 300);
    // the next step needs another full healthy period
    for (int i = 0; i < 15; ++i, t += 1000) EXPECT_EQ(a.update(t, sample(1)), 0);

    for (int i = 0; i < 2000; ++i, t += 1000) a.update(t, sample(1));
    EXPECT_EQ(a.currentKbps(), 6000);   // fully recovered, clamped to the target
}

TEST(AdaptiveBitrate, NeutralSamplesNeitherReduceNorRecover) {
    AdaptiveBitrate a(cfg());
    std::int64_t t = 0;
    for (int i = 0; i < 4; ++i, t += 1000) a.update(t, sample(80));
    const int reduced = a.currentKbps();
    // queue sits at 20%: not stressed (<25% growth threshold when flat), not healthy (>15%)
    for (int i = 0; i < 120; ++i, t += 1000) EXPECT_EQ(a.update(t, sample(18)), 0);
    EXPECT_EQ(a.currentKbps(), reduced);
}

TEST(AdaptiveBitrate, ResetGoesBackToTarget) {
    AdaptiveBitrate a(cfg());
    for (int i = 0; i < 4; ++i) a.update(i * 1000, sample(80));
    ASSERT_LT(a.currentKbps(), 6000);
    a.reset();
    EXPECT_EQ(a.currentKbps(), 6000);
}

TEST(AdaptiveBitrate, TinyTargetKeepsAValidFloor) {
    AdaptiveBitrate a(cfg(300));
    EXPECT_EQ(a.minKbps(), 120);
    for (int i = 0; i < 100; ++i) a.update(i * 1000, sample(80));
    EXPECT_GE(a.currentKbps(), 120);
}
