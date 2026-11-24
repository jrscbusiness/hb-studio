#include <gtest/gtest.h>

#include "marketplace/semver.hpp"

using namespace hb::marketplace;

TEST(MarketplaceSemver, ParsesValidAndRejectsInvalid) {
    SemVer v;
    EXPECT_TRUE(parseSemVer("1.2.3", v));
    EXPECT_EQ(v.major, 1);
    EXPECT_EQ(v.minor, 2);
    EXPECT_EQ(v.patch, 3);
    EXPECT_TRUE(parseSemVer("0.3.0+abc1234", v));          // the app's own display version
    EXPECT_TRUE(parseSemVer("1.0.0-rc.1", v));
    EXPECT_EQ(v.pre, "rc.1");
    for (const char* bad : {"", "1", "1.2", "1.2.", "v1.2.3", "1.2.3.4", "a.b.c", "1.2.3-", "1.2.3+", "1.2.3-a..b", "1.2.x", " 1.2.3", "1.2.3 ",
                            "1234567.0.0", "-1.0.0", "1.2.3-é"})
        EXPECT_FALSE(parseSemVer(bad, v)) << bad;
}

TEST(MarketplaceSemver, Ordering) {
    auto cmp = [](const char* a, const char* b) {
        SemVer x, y;
        EXPECT_TRUE(parseSemVer(a, x));
        EXPECT_TRUE(parseSemVer(b, y));
        return compareSemVer(x, y);
    };
    EXPECT_LT(cmp("1.2.3", "1.2.4"), 0);
    EXPECT_LT(cmp("1.9.0", "1.10.0"), 0);          // numeric, not lexicographic
    EXPECT_LT(cmp("1.99.99", "2.0.0"), 0);
    EXPECT_EQ(cmp("1.0.0", "1.0.0+build5"), 0);    // build metadata ignored
    EXPECT_LT(cmp("1.0.0-alpha", "1.0.0"), 0);     // prerelease < release
    EXPECT_LT(cmp("1.0.0-alpha", "1.0.0-alpha.1"), 0);
    EXPECT_LT(cmp("1.0.0-alpha.1", "1.0.0-beta"), 0);
    EXPECT_LT(cmp("1.0.0-2", "1.0.0-10"), 0);
    EXPECT_LT(cmp("1.0.0-1", "1.0.0-a"), 0);       // numeric identifiers first
    EXPECT_GT(cmp("1.0.0-rc.1", "1.0.0-beta.11"), 0);
}

TEST(MarketplaceSemver, UpdateAvailableLogic) {
    EXPECT_TRUE(isNewerVersion("1.1.0", "1.0.9"));
    EXPECT_FALSE(isNewerVersion("1.0.0", "1.0.0"));
    EXPECT_FALSE(isNewerVersion("1.0.0", "1.0.1"));         // never "update" to an older version
    EXPECT_FALSE(isNewerVersion("garbage", "1.0.0"));       // invalid input never offers an update
    EXPECT_FALSE(isNewerVersion("2.0.0", "garbage"));
}
