#include <gtest/gtest.h>

#include <chrono>
#include <string>

#include "core/redact.hpp"

using hb::core::redactSecrets;

namespace {
bool contains(const std::string& s, const char* needle) { return s.find(needle) != std::string::npos; }
}

TEST(Redact, RtspUrlCredentials) {
    auto r = redactSecrets("connecting rtsp://admin:S3cr3t!@192.168.15.100:554/stream");
    EXPECT_FALSE(contains(r, "S3cr3t"));
    EXPECT_FALSE(contains(r, "admin"));
    EXPECT_TRUE(contains(r, "192.168.15.100:554/stream"));
}

TEST(Redact, UrlWithUserOnly) {
    auto r = redactSecrets("open rtsp://operator@10.0.0.5/live");
    EXPECT_FALSE(contains(r, "operator"));
    EXPECT_TRUE(contains(r, "10.0.0.5/live"));
}

TEST(Redact, KeyValueForms) {
    EXPECT_FALSE(contains(redactSecrets("password=hunter2 user=x"), "hunter2"));
    EXPECT_FALSE(contains(redactSecrets("Passphrase: abc123xyz"), "abc123xyz"));
    EXPECT_FALSE(contains(redactSecrets(R"({"token":"eyJhbGci"})"), "eyJhbGci"));
    EXPECT_FALSE(contains(redactSecrets("api_key=AKIA999"), "AKIA999"));
    EXPECT_TRUE(contains(redactSecrets("password=hunter2 user=x"), "user=x"));
}

TEST(Redact, AuthorizationHeader) {
    auto r = redactSecrets("GET / HTTP/1.1\nAuthorization: Digest username=\"a\", response=\"deadbeef\"\nHost: x");
    EXPECT_FALSE(contains(r, "deadbeef"));
    EXPECT_FALSE(contains(r, "username"));
    EXPECT_TRUE(contains(r, "Host: x"));
}

TEST(Redact, WsSecurityPassword) {
    auto r = redactSecrets("<wsse:Password Type=\"digest\">QUJDREVG</wsse:Password>");
    EXPECT_FALSE(contains(r, "QUJDREVG"));
}

TEST(Redact, RtmpStreamKey) {
    auto r = redactSecrets("push rtmp://live.example.com/app/abcd-1234-efgh");
    EXPECT_FALSE(contains(r, "abcd-1234"));
    EXPECT_TRUE(contains(r, "live.example.com/app/"));
}

TEST(Redact, PlainTextUntouched) {
    const std::string s = "CAM 03 OFFLINE reconnecting in 2s, 1920x1080@30";
    EXPECT_EQ(redactSecrets(s), s);
}

// ---- Regression tests for every leak proven by docs/REVIEW-1.md (A3/A4) ----
namespace {
bool has(const std::string& s, const char* n) { return s.find(n) != std::string::npos; }
}

TEST(RedactRegression, PasswordWithAtSign) {
    auto r = redactSecrets("rtsp://admin:p@ss@10.0.0.1/s");
    EXPECT_FALSE(has(r, "ss@")); EXPECT_FALSE(has(r, "admin")); EXPECT_TRUE(has(r, "10.0.0.1/s"));
}
TEST(RedactRegression, PasswordWithSlash) {
    auto r = redactSecrets("rtsp://admin:pa/ss@10.0.0.1/s");
    EXPECT_FALSE(has(r, "pa/ss")); EXPECT_FALSE(has(r, "admin")); EXPECT_TRUE(has(r, "10.0.0.1/s"));
}
TEST(RedactRegression, PathWithAtIsNotUserinfo) {
    const std::string s = "GET http://host:8080/users/bob@example.com ok";
    EXPECT_EQ(redactSecrets(s), s);
}
TEST(RedactRegression, MultipleUrlsInOneMessage) {
    auto r = redactSecrets("a rtsp://u1:p1@h1/x b rtsp://u2:p2@h2/y c");
    EXPECT_FALSE(has(r, "p1")); EXPECT_FALSE(has(r, "p2")); EXPECT_TRUE(has(r, "h1/x")); EXPECT_TRUE(has(r, "h2/y"));
}
TEST(RedactRegression, Cookies) {
    EXPECT_FALSE(has(redactSecrets("Cookie: session=SESSVAL999; other=1"), "SESSVAL999"));
    EXPECT_FALSE(has(redactSecrets("Set-Cookie: sid=COOKIEVAL42; HttpOnly"), "COOKIEVAL42"));
}
TEST(RedactRegression, JsonAuthorizationKey) {
    EXPECT_FALSE(has(redactSecrets(R"({"Authorization":"Bearer ABCDEF123"})"), "ABCDEF123"));
    EXPECT_FALSE(has(redactSecrets(R"({"authorization": "Basic QUJD"})"), "QUJD"));
}
TEST(RedactRegression, CompoundKeys) {
    for (const char* k : {"access_token=ACC123", "refresh_token=REF123", "client_secret=CS123", "secretKey=SK123",
                          "db_password=DBP123", "user_password=UP123", "passwordHash=PH999", "stream_key=SK999"}) {
        auto r = redactSecrets(k);
        const std::string v = std::string(k).substr(std::string(k).find('=') + 1);
        EXPECT_FALSE(has(r, v.c_str())) << k << " -> " << r;
    }
}
TEST(RedactRegression, ValuesWithSpecialCharacters) {
    EXPECT_FALSE(has(redactSecrets(R"({"password":"hun ter2 more"})"), "ter2"));
    EXPECT_FALSE(has(redactSecrets(R"(password="two words")"), "words"));
    EXPECT_FALSE(has(redactSecrets("password: two words here"), "words"));
    EXPECT_FALSE(has(redactSecrets("password=a,b;c"), "b;c"));
    EXPECT_FALSE(has(redactSecrets(R"(password="a;b")"), "a;b"));
    EXPECT_FALSE(has(redactSecrets(R"({"token":"abc\"def"})"), "def"));
}
TEST(RedactRegression, QueryStringSecrets) {
    for (const char* q : {"http://h/p?access_token=XYZ123", "http://h/p?auth=XYZ123", "http://h/p?key=XYZ123",
                          "http://h/p?sig=XYZ123", "srt://h:9000?streamid=XYZ123"}) {
        auto r = redactSecrets(q);
        EXPECT_FALSE(has(r, "XYZ123")) << q << " -> " << r;
    }
    EXPECT_TRUE(has(redactSecrets("http://h/p?a=1&access_token=X&b=2"), "b=2"));
}
TEST(RedactRegression, RtmpKeyDirectlyAfterHost) {
    EXPECT_FALSE(has(redactSecrets("rtmp://live.example.com/KEYONLY789"), "KEYONLY789"));
    EXPECT_TRUE(has(redactSecrets("rtmp://live.example.com/KEYONLY789"), "live.example.com"));
}
TEST(RedactRegression, BareBearerAndBasic) {
    EXPECT_FALSE(has(redactSecrets("sent Basic YWRtaW46aHVudGVyMg== to server"), "YWRtaW46"));
    EXPECT_FALSE(has(redactSecrets("hdr Bearer eyJhbGciOiJIUzI1NiJ9.abc.def"), "eyJhbGci"));
    EXPECT_EQ(redactSecrets("a basic setup is ready"), "a basic setup is ready");
}
TEST(RedactRegression, NormalTextAndFalsePositives) {
    for (const char* s : {"author=alice done", "authority example.com", "keyframe interval 30", "compass bypass passed",
                          "primary stream started 1920x1080@30", "monkey business"})
        EXPECT_EQ(redactSecrets(s), s) << s;
}
TEST(RedactRegression, HugeInputsAreFastAndSafe) {
    const auto t0 = std::chrono::steady_clock::now();
    EXPECT_NO_THROW(redactSecrets(std::string(100000, 'a')));
    EXPECT_NO_THROW(redactSecrets(std::string(100000, '=')));
    EXPECT_NO_THROW(redactSecrets(std::string(50000, ':') + "@"));
    std::string urls;
    for (int i = 0; i < 5000; ++i) urls += "x://";
    EXPECT_NO_THROW(redactSecrets(urls));
    auto r = redactSecrets(std::string(100000, 'a'));
    EXPECT_NE(r.find("[truncado]"), std::string::npos);
    EXPECT_LT(std::chrono::steady_clock::now() - t0, std::chrono::seconds(2));
}
