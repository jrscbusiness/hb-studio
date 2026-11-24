#include <gtest/gtest.h>

#include <cstring>
#include <set>

#include "remote/auth.hpp"

using namespace hb::remote;
using namespace std::chrono;

namespace {
// Deterministic "random" source for tests only (production uses the OS CSPRNG).
class FakeRandom : public IRandom {
public:
    bool bytes(std::uint8_t* b, std::size_t n) override {
        if (fail) return false;
        for (std::size_t i = 0; i < n; ++i) b[i] = static_cast<std::uint8_t>((counter_ = counter_ * 1103515245u + 12345u) >> 16);
        return true;
    }
    bool fail = false;

private:
    std::uint32_t counter_ = 7;
};
const TimePoint T0 = TimePoint{} + hours(1);
}  // namespace

TEST(RemoteAuth, SystemRandomWorks) {
    std::uint8_t a[32] = {0}, b[32] = {0};
    ASSERT_TRUE(systemRandom().bytes(a, 32));
    ASSERT_TRUE(systemRandom().bytes(b, 32));
    EXPECT_NE(0, std::memcmp(a, b, 32));
}

TEST(RemoteAuth, PinIsSixDigits) {
    AuthManager am(systemRandom());
    for (int i = 0; i < 50; ++i) {
        am.newPin();
        const std::string p = am.pin();
        ASSERT_EQ(p.size(), 6u);
        for (char c : p) EXPECT_TRUE(c >= '0' && c <= '9');
    }
}

TEST(RemoteAuth, PairThenTokenLogin) {
    FakeRandom r;
    AuthManager am(r);
    const std::string pin = am.pin();
    auto o = am.pair(pin, "iPhone do Rafael", "192.168.0.9", T0);
    ASSERT_EQ(o.result, PairResult::Ok);
    EXPECT_EQ(o.token.size(), 64u);
    EXPECT_NE(am.pin(), pin);  // single-use PIN
    auto id = am.authenticate(o.token, T0 + seconds(5));
    ASSERT_TRUE(id.has_value());
    EXPECT_EQ(*id, o.sessionId);
    EXPECT_FALSE(am.authenticate(std::string(64, 'a'), T0));
    EXPECT_FALSE(am.authenticate("", T0));
    EXPECT_FALSE(am.authenticate(o.token + "0", T0));
    auto list = am.sessions(T0);
    ASSERT_EQ(list.size(), 1u);
    EXPECT_EQ(list[0].name, "iPhone do Rafael");
    EXPECT_EQ(list[0].ip, "192.168.0.9");
}

TEST(RemoteAuth, TokensAreUniqueAndNotStoredRaw) {
    AuthManager am(systemRandom(), AuthConfig{.rotatePinOnPair = false});
    std::set<std::string> toks;
    for (int i = 0; i < 10; ++i) {
        auto o = am.pair(am.pin(), "d", "10.0.0.1", T0);
        ASSERT_EQ(o.result, PairResult::Ok);
        toks.insert(o.token);
    }
    EXPECT_EQ(toks.size(), 10u);
}

TEST(RemoteAuth, BruteForceBurnsPinAndLocksOutWithBackoff) {
    FakeRandom r;
    AuthManager am(r);
    const std::string pin = am.pin();
    const std::string wrong = pin == "000000" ? "000001" : "000000";
    for (int i = 0; i < 4; ++i) {
        auto o = am.pair(wrong, "x", "ip", T0);
        EXPECT_EQ(o.result, PairResult::BadPin);
        EXPECT_EQ(o.attemptsLeft, 4 - i);
    }
    EXPECT_EQ(am.pin(), pin);
    EXPECT_EQ(am.pair(wrong, "x", "ip", T0).result, PairResult::BadPin);  // 5th failure
    EXPECT_NE(am.pin(), pin);                                               // PIN replaced
    const std::string pin2 = am.pin();
    // Locked out: even the correct PIN is refused and not even evaluated.
    auto l = am.pair(pin2, "x", "ip", T0 + seconds(10));
    EXPECT_EQ(l.result, PairResult::Locked);
    EXPECT_GT(l.retryAfter.count(), 15000);
    EXPECT_EQ(am.pin(), pin2);
    EXPECT_GT(am.lockedFor(T0 + seconds(10)).count(), 0);
    // After 30 s it works again...
    for (int i = 0; i < 5; ++i) am.pair("999999x", "x", "ip", T0 + seconds(31));
    // ...and the second lockout is twice as long (60 s).
    auto l2 = am.pair(am.pin(), "x", "ip", T0 + seconds(31) + seconds(30));
    EXPECT_EQ(l2.result, PairResult::Locked);
    auto ok = am.pair(am.pin(), "x", "ip", T0 + seconds(31) + seconds(61));
    EXPECT_EQ(ok.result, PairResult::Ok);
}

TEST(RemoteAuth, LockoutIsCapped) {
    FakeRandom r;
    AuthConfig c;
    c.lockoutBase = seconds(30);
    c.lockoutMax = seconds(100);
    AuthManager am(r, c);
    TimePoint t = T0;
    for (int round = 0; round < 8; ++round) {
        for (int i = 0; i < 5; ++i) am.pair("bad", "x", "ip", t);
        EXPECT_LE(am.lockedFor(t).count(), 100000);
        t += seconds(101);
    }
}

TEST(RemoteAuth, SessionsExpireAbsoluteAndIdle) {
    FakeRandom r;
    AuthConfig c;
    c.sessionAbsolute = minutes(60);
    c.sessionIdle = minutes(10);
    AuthManager am(r, c);
    auto a = am.pair(am.pin(), "a", "ip", T0);
    auto b = am.pair(am.pin(), "b", "ip", T0);
    ASSERT_EQ(a.result, PairResult::Ok);
    ASSERT_EQ(b.result, PairResult::Ok);
    // b stays active, a goes idle.
    for (int m = 1; m <= 5; ++m) EXPECT_TRUE(am.authenticate(b.token, T0 + minutes(m * 5)).has_value());
    EXPECT_FALSE(am.valid(a.sessionId, T0 + minutes(25)));
    EXPECT_FALSE(am.authenticate(a.token, T0 + minutes(25)));
    EXPECT_TRUE(am.valid(b.sessionId, T0 + minutes(25)));
    // Absolute cap even if active.
    for (int m = 30; m <= 60; m += 5) am.touch(b.sessionId, T0 + minutes(m));
    EXPECT_FALSE(am.valid(b.sessionId, T0 + minutes(61)));
}

TEST(RemoteAuth, RevokeAndRevokeAll) {
    FakeRandom r;
    AuthManager am(r);
    auto a = am.pair(am.pin(), "a", "ip", T0);
    auto b = am.pair(am.pin(), "b", "ip", T0);
    EXPECT_TRUE(am.revoke(a.sessionId));
    EXPECT_FALSE(am.revoke(a.sessionId));
    EXPECT_FALSE(am.authenticate(a.token, T0));
    EXPECT_TRUE(am.authenticate(b.token, T0));
    const std::string pinBefore = am.pin();
    am.revokeAll();
    EXPECT_FALSE(am.authenticate(b.token, T0));
    EXPECT_TRUE(am.sessions(T0).empty());
    EXPECT_NE(am.pin(), pinBefore);
}

TEST(RemoteAuth, SessionLimit) {
    FakeRandom r;
    AuthConfig c;
    c.maxSessions = 2;
    AuthManager am(r, c);
    EXPECT_EQ(am.pair(am.pin(), "a", "ip", T0).result, PairResult::Ok);
    EXPECT_EQ(am.pair(am.pin(), "b", "ip", T0).result, PairResult::Ok);
    EXPECT_EQ(am.pair(am.pin(), "c", "ip", T0).result, PairResult::TooManySessions);
}

TEST(RemoteAuth, RandomFailureNeverMintsSecrets) {
    FakeRandom r;
    AuthManager am(r);
    r.fail = true;
    am.newPin();
    EXPECT_TRUE(am.pin().empty());
    EXPECT_EQ(am.pair("", "x", "ip", T0).result, PairResult::RandomFailure);
    EXPECT_EQ(am.pair("123456", "x", "ip", T0).result, PairResult::RandomFailure);
}

TEST(RemoteAuth, ConstantTimeEqualsAndSanitize) {
    EXPECT_TRUE(AuthManager::constantTimeEquals("abc", "abc"));
    EXPECT_FALSE(AuthManager::constantTimeEquals("abc", "abd"));
    EXPECT_FALSE(AuthManager::constantTimeEquals("abc", "abcd"));
    EXPECT_FALSE(AuthManager::constantTimeEquals("", "a"));
    EXPECT_TRUE(AuthManager::constantTimeEquals("", ""));
    EXPECT_EQ(AuthManager::sanitizeName("  Tablet <b>\"X\"\n "), "Tablet bX");
    EXPECT_EQ(AuthManager::sanitizeName("\x01\x02"), "Dispositivo");
    EXPECT_EQ(AuthManager::sanitizeName(std::string(500, 'a')).size(), 40u);
    EXPECT_EQ(AuthManager::sanitizeName("\xC0\xAF"), "Dispositivo");
}
