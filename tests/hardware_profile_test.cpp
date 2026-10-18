#include <gtest/gtest.h>

#include "core/hardware_profile.hpp"

using namespace hb::core;
constexpr std::uint64_t GiB = 1024ull * 1024 * 1024;

TEST(Hardware, ClassifyTiers) {
    EXPECT_EQ(classify({2, 4 * GiB, "", 0, false, false, false}), Tier::Low);
    EXPECT_EQ(classify({4, 8 * GiB, "", 0, false, false, false}), Tier::Medium);
    EXPECT_EQ(classify({8, 16 * GiB, "", 4 * GiB, true, true, true}), Tier::High);
    EXPECT_EQ(classify({16, 32 * GiB, "", 12 * GiB, true, true, true}), Tier::Pro);
}

TEST(Hardware, WithoutHwDecodeCappedAtMedium) {
    EXPECT_EQ(classify({32, 64 * GiB, "", 24 * GiB, false, false, false}), Tier::Medium);
}

TEST(Hardware, DetectReturnsSomething) {
    auto hw = detectHardware();
    EXPECT_GT(hw.logicalCores, 0u);
    EXPECT_GT(hw.ramBytes, 0u);
}

TEST(Hardware, ManualOverrideBeatsAutomatic) {
    HardwareInfo weak{2, 4 * GiB, "", 0, false, false, false};
    HardwareProfileSetting s;
    EXPECT_EQ(s.effective(weak), Tier::Low);
    s.automatic = false;
    s.manual = Tier::Pro;
    EXPECT_EQ(s.effective(weak), Tier::Pro);
}

TEST(Hardware, PresetsAreMonotonic) {
    int prev = 0;
    for (auto t : {Tier::Low, Tier::Medium, Tier::High, Tier::Pro}) {
        const auto& p = presetFor(t);
        EXPECT_EQ(p.tier, t);
        EXPECT_GE(p.hardMaxSources, p.recommendedSources);
        EXPECT_GE(p.recommendedSources, prev);
        prev = p.recommendedSources;
    }
}

TEST(Hardware, BudgetVerdicts) {
    EXPECT_EQ(checkSourceBudget(Tier::Medium, 8).verdict, BudgetVerdict::Ok);
    auto w = checkSourceBudget(Tier::Medium, 9);
    EXPECT_EQ(w.verdict, BudgetVerdict::Warning);
    EXPECT_NE(w.message.find("PERFORMANCE WARNING"), std::string::npos);
    EXPECT_EQ(checkSourceBudget(Tier::Medium, 11).verdict, BudgetVerdict::Blocked);
    EXPECT_EQ(checkSourceBudget(Tier::Medium, 11, true).verdict, BudgetVerdict::Warning);
}

TEST(Hardware, CommonOutputPresetsAndLatencyModes) {
    bool has1080p30 = false;
    for (const auto& p : commonOutputPresets())
        if (std::string(p.name) == "1080p30") {
            has1080p30 = true;
            EXPECT_EQ(p.resolution.width, 1920);
            EXPECT_EQ(p.fps.value(), 30.0);
        }
    EXPECT_TRUE(has1080p30);
    EXPECT_LT(latencyParams(LatencyMode::Low).networkBufferMs,
              latencyParams(LatencyMode::Balanced).networkBufferMs);
    EXPECT_LT(latencyParams(LatencyMode::Balanced).networkBufferMs,
              latencyParams(LatencyMode::Stable).networkBufferMs);
}
