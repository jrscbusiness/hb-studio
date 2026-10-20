#include <gtest/gtest.h>

#include "rtsp/auth.hpp"

using namespace hb::rtsp;

TEST(Auth, DigestMatchesRfc2617Example) {
    Challenge c;
    c.scheme = Challenge::Scheme::Digest;
    c.realm = "testrealm@host.com";
    c.nonce = "dcd98b7102dd2f0e8b11d0f600bfb0c093";
    EXPECT_EQ(digestResponse(c, {"Mufasa", "Circle Of Life"}, "GET", "/dir/index.html", "auth", "00000001", "0a4f113b"),
              "6629fae49393a05397450978507c4ef1");
}

TEST(Auth, DigestMatchesRfc7616Md5Example) {
    Challenge c;
    c.realm = "http-auth@example.org";
    c.nonce = "7ypf/xlj9XXwfDPEoM4URrv/xwf94BcCAzFZH4GiTo0v";
    EXPECT_EQ(digestResponse(c, {"Mufasa", "Circle of Life"}, "GET", "/dir/index.html", "auth", "00000001",
                             "f2/wE4q74E6zIJEtWaHKaf5wv/H5QzzpXusqGemxURZJ"),
              "8ca523f5e9506fed4657c9700eebdbec");
}

TEST(Auth, DigestLegacyWithoutQopRegression) {
    // Value computed independently (python hashlib) for the RFC 2069 style H(H1:nonce:H2) formula.
    Challenge c;
    c.realm = "testrealm@host.com";
    c.nonce = "dcd98b7102dd2f0e8b11d0f600bfb0c093";
    EXPECT_EQ(digestResponse(c, {"Mufasa", "Circle Of Life"}, "GET", "/dir/index.html", "", "", ""),
              "670fd8c2df070c60b045671b8b24ff02");
}

TEST(Auth, BasicHeader) {
    EXPECT_EQ(basicHeaderValue({"Aladdin", "open sesame"}), "Basic QWxhZGRpbjpvcGVuIHNlc2FtZQ==");
}

TEST(Auth, ParseChallengesMultipleSchemesInOneHeader) {
    auto cs = parseChallenges({"Basic realm=\"cam\", Digest realm=\"cam, hd\", nonce=\"abc\", qop=\"auth,auth-int\", "
                               "opaque=\"o\", stale=TRUE, algorithm=MD5"});
    ASSERT_EQ(cs.size(), 2u);
    EXPECT_EQ(cs[0].scheme, Challenge::Scheme::Basic);
    EXPECT_EQ(cs[0].realm, "cam");
    EXPECT_EQ(cs[1].scheme, Challenge::Scheme::Digest);
    EXPECT_EQ(cs[1].realm, "cam, hd");  // comma inside quotes preserved
    EXPECT_EQ(cs[1].nonce, "abc");
    EXPECT_EQ(cs[1].opaque, "o");
    EXPECT_TRUE(cs[1].stale);
    ASSERT_EQ(cs[1].qop.size(), 2u);
    EXPECT_EQ(cs[1].qop[1], "auth-int");
}

TEST(Auth, ParseChallengesSeparateHeadersAndUnknownScheme) {
    auto cs = parseChallenges({"Negotiate", "Digest realm=\"r\", nonce=\"n\"", "Bearer realm=\"x\""});
    ASSERT_EQ(cs.size(), 1u);
    EXPECT_EQ(cs[0].scheme, Challenge::Scheme::Digest);
}

TEST(Auth, AuthenticatorPrefersDigestAndCountsNonces) {
    Authenticator a({"u", "p"}, [] { return std::string("cn"); });
    EXPECT_TRUE(a.header("DESCRIBE", "rtsp://h/s").empty());  // nothing before a challenge
    ASSERT_TRUE(a.setChallenge({"Basic realm=\"r\"", "Digest realm=\"r\", nonce=\"n1\", qop=\"auth\""}));
    std::string h1 = a.header("DESCRIBE", "rtsp://h/s");
    std::string h2 = a.header("SETUP", "rtsp://h/s/t1");
    EXPECT_EQ(h1.rfind("Digest ", 0), 0u);
    EXPECT_NE(h1.find("nc=00000001"), std::string::npos);
    EXPECT_NE(h2.find("nc=00000002"), std::string::npos);
    EXPECT_NE(h1.find("uri=\"rtsp://h/s\""), std::string::npos);
    EXPECT_NE(h1.find("cnonce=\"cn\""), std::string::npos);
    EXPECT_EQ(h1.find("p\""), std::string::npos);  // password is never in the header
}

TEST(Auth, AuthenticatorFallsBackToBasic) {
    Authenticator a({"u", "p"}, nullptr);
    ASSERT_TRUE(a.setChallenge({"Basic realm=\"r\""}));
    EXPECT_EQ(a.header("DESCRIBE", "x"), basicHeaderValue({"u", "p"}));
}

TEST(Auth, UnsupportedOrUnusableChallengesRejected) {
    Authenticator a({"u", "p"}, nullptr);
    EXPECT_FALSE(a.setChallenge({"Digest realm=\"r\", nonce=\"n\", algorithm=SHA-256, qop=\"auth\""}));
    EXPECT_FALSE(a.setChallenge({"Digest realm=\"r\", nonce=\"n\", qop=\"auth-int\""}));
    EXPECT_FALSE(a.setChallenge({"Bearer x"}));
    Authenticator noCred({"", ""}, nullptr);
    EXPECT_FALSE(noCred.setChallenge({"Basic realm=\"r\""}));
}

TEST(Auth, DigestWithoutQopUsesLegacyForm) {
    Authenticator a({"Mufasa", "Circle Of Life"}, nullptr);
    ASSERT_TRUE(a.setChallenge({"Digest realm=\"testrealm@host.com\", nonce=\"dcd98b7102dd2f0e8b11d0f600bfb0c093\""}));
    std::string h = a.header("GET", "/dir/index.html");
    EXPECT_NE(h.find("response=\"670fd8c2df070c60b045671b8b24ff02\""), std::string::npos);
    EXPECT_EQ(h.find("qop="), std::string::npos);
}
