#include <gtest/gtest.h>

#include "perf/quality_governor.hpp"

using namespace hb::perf;

namespace {

struct Sim {
    QualityGovernor g;
    double t = 100;
    std::uint64_t skipped = 0;
    explicit Sim(int user = 3, GovernorConfig cfg = {}) : g(cfg, 4, user, [this] { return t; }) {}
    // Advances one second with the given fps / newly skipped ticks.
    int sec(double fps, std::uint64_t newSkips = 0) {
        t += 1.0;
        skipped += newSkips;
        return g.update({fps, skipped, 30});
    }
    int run(int n, double fps, std::uint64_t newSkips = 0) {
        int l = 0;
        for (int i = 0; i < n; ++i) l = sec(fps, newSkips);
        return l;
    }
};

}  // namespace

TEST(QualityGovernor, HealthyClockNeverChangesTheLevel) {
    Sim s;
    EXPECT_EQ(s.run(120, 30.0), 3);
    EXPECT_FALSE(s.g.reduced());
    EXPECT_EQ(s.g.stepsDown(), 0);
}

TEST(QualityGovernor, StepsDownOneLevelAtATimeWithCooldown) {
    Sim s;
    s.sec(30);                 // baseline sample
    EXPECT_EQ(s.sec(20), 3);   // one bad sample is not enough
    EXPECT_EQ(s.sec(20), 2);   // two consecutive: step down
    EXPECT_TRUE(s.g.reduced());
    EXPECT_EQ(s.sec(20), 2);   // streak restarted + cooldown
    EXPECT_EQ(s.sec(20), 2);
    EXPECT_EQ(s.sec(20), 1);   // cooldown (3 s) elapsed, 2 more bad samples
    s.run(30, 20);
    EXPECT_EQ(s.g.level(), 0);  // never below the lightest preset
    EXPECT_EQ(s.run(10, 5), 0);
}

TEST(QualityGovernor, SkippedTicksAloneCountAsTrouble) {
    Sim s;
    s.sec(30);
    s.sec(30, 2);
    EXPECT_EQ(s.sec(30, 1), 2);   // fps looks fine but the clock dropped ticks
}

TEST(QualityGovernor, BandBetweenBadAndGoodResetsStreaks) {
    Sim s;
    s.sec(30);
    for (int i = 0; i < 10; ++i) {
        s.sec(20);                  // bad
        EXPECT_EQ(s.sec(28.5), 3);  // 95% of target: neither bad nor good, resets the bad streak
    }
    EXPECT_EQ(s.g.level(), 3);
}

TEST(QualityGovernor, ClimbsBackOnlyAfterSustainedHealthAndNeverAboveUserLevel) {
    Sim s;
    s.sec(30);
    s.run(10, 15);   // drop a few levels
    const int low = s.g.level();
    ASSERT_LT(low, 3);
    // 9 good seconds are not enough (needs 10 AND the 15 s up cooldown).
    for (int i = 0; i < 9; ++i) EXPECT_EQ(s.sec(30), low);
    int l = low;
    for (int i = 0; i < 20 && l == low; ++i) l = s.sec(30);
    EXPECT_EQ(l, low + 1);
    s.run(400, 30);
    EXPECT_EQ(s.g.level(), 3);          // back at the operator's choice
    EXPECT_FALSE(s.g.reduced());
    s.run(100, 30);
    EXPECT_EQ(s.g.level(), 3);          // and never above it
}

TEST(QualityGovernor, FlappingDoublesTheRecoveryRequirement) {
    Sim s;
    s.sec(30);
    s.run(8, 15);
    const int low = s.g.level();
    ASSERT_LE(low, 2);
    // Recover one step, then trouble returns inside the flap window.
    int l = low;
    for (int i = 0; i < 40 && l == low; ++i) l = s.sec(30);
    ASSERT_EQ(l, low + 1);
    s.run(8, 15);
    ASSERT_LT(s.g.level(), low + 1);
    const int low2 = s.g.level();
    // 10 good seconds used to be enough; now 20 are needed (plus the cooldown).
    for (int i = 0; i < 19; ++i) EXPECT_EQ(s.sec(30), low2) << i;
    int steps = 0;
    for (int i = 0; i < 40; ++i)
        if (s.sec(30) > low2) { ++steps; break; }
    EXPECT_EQ(steps, 1);
}

TEST(QualityGovernor, StoppedClockIsIgnored) {
    Sim s;
    s.sec(30);
    EXPECT_EQ(s.run(30, 0.0), 3);   // fps 0 = program not running: no evidence either way
}

TEST(QualityGovernor, DisablingRestoresTheOperatorLevelImmediately) {
    Sim s;
    s.sec(30);
    s.run(20, 10);
    ASSERT_TRUE(s.g.reduced());
    s.g.setEnabled(false);
    EXPECT_EQ(s.g.level(), 3);
    EXPECT_FALSE(s.g.reduced());
    EXPECT_EQ(s.run(30, 5), 3);   // disabled: bad samples do nothing
    s.g.setEnabled(true);
    EXPECT_EQ(s.g.level(), 3);
}

TEST(QualityGovernor, OperatorChoiceBecomesTheNewCeilingAndIsAppliedAtOnce) {
    Sim s(3);
    s.sec(30);
    s.run(20, 10);
    ASSERT_LT(s.g.level(), 3);
    s.g.setUserLevel(1);
    EXPECT_EQ(s.g.level(), 1);
    EXPECT_EQ(s.g.userLevel(), 1);
    s.run(200, 30);
    EXPECT_EQ(s.g.level(), 1);   // never climbs above the new choice
    s.g.setUserLevel(99);        // out of range is clamped
    EXPECT_EQ(s.g.level(), 3);
    s.g.setUserLevel(-4);
    EXPECT_EQ(s.g.level(), 0);
}

TEST(QualityGovernor, SamplesCloserThanMinPeriodAreIgnored) {
    GovernorConfig cfg;
    QualityGovernor g(cfg, 4, 3, nullptr);
    std::uint64_t skipped = 0;
    g.update({30, skipped, 30}, 10.0);
    for (int i = 0; i < 50; ++i) g.update({5, skipped, 30}, 10.0 + 0.01 * (i + 1));   // 15 Hz-ish calls within one second
    EXPECT_EQ(g.level(), 3);
}

TEST(QualityGovernor, SkippedCounterGoingBackwardsIsHarmless) {
    Sim s;
    s.sec(30, 0);
    s.skipped = 1000;
    s.sec(30);
    s.skipped = 5;   // engine restarted
    EXPECT_EQ(s.sec(30), 3);
}

TEST(QualityGovernor, ProgramOutputResolutionIsNotInItsVocabulary) {
    // The governor only ever returns a preview preset index within [0, levels-1].
    Sim s;
    s.sec(30);
    for (int i = 0; i < 100; ++i) {
        const int l = s.sec(i % 7 == 0 ? 3.0 : 30.0, i % 3);
        EXPECT_GE(l, 0);
        EXPECT_LE(l, 3);
    }
}
