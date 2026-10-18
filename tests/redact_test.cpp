#include <gtest/gtest.h>

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
