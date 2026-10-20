#include <gtest/gtest.h>

#include <string>

#include "onvif/crypto.hpp"
#include "onvif/time_util.hpp"
#include "onvif/url.hpp"

using namespace hb::onvif;

namespace {
std::string hexSha1(const std::string& s) {
    const auto d = sha1(s);
    return toHex(d.data(), d.size());
}
}  // namespace

TEST(Sha1, KnownVectors) {  // FIPS 180 / RFC 3174 test vectors
    EXPECT_EQ(hexSha1(""), "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    EXPECT_EQ(hexSha1("abc"), "a9993e364706816aba3e25717850c26c9cd0d89d");
    EXPECT_EQ(hexSha1("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"), "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
    EXPECT_EQ(hexSha1("The quick brown fox jumps over the lazy dog"), "2fd4e1c67a2d28fced849ee1bb76e7391b93eb12");
    EXPECT_EQ(hexSha1(std::string(1000000, 'a')), "34aa973cd4c4daa4f61eeb2bdbad27316534016f");
}

TEST(Sha1, PaddingBoundaries) {
    // Lengths around the 55/56/63/64 byte padding boundaries must not crash and must differ.
    std::string prev;
    for (int n : {54, 55, 56, 57, 63, 64, 65, 119, 120, 128}) {
        const auto h = hexSha1(std::string(static_cast<std::size_t>(n), 'x'));
        EXPECT_EQ(h.size(), 40u);
        EXPECT_NE(h, prev);
        prev = h;
    }
    // Known: 55 and 64 'a' bytes (computed with openssl).
    EXPECT_EQ(hexSha1(std::string(64, 'a')), "0098ba824b5c16427bd7a1122a5a442a25ec644d");
}

TEST(Base64, Rfc4648Vectors) {
    EXPECT_EQ(base64Encode(""), "");
    EXPECT_EQ(base64Encode("f"), "Zg==");
    EXPECT_EQ(base64Encode("fo"), "Zm8=");
    EXPECT_EQ(base64Encode("foo"), "Zm9v");
    EXPECT_EQ(base64Encode("foob"), "Zm9vYg==");
    EXPECT_EQ(base64Encode("fooba"), "Zm9vYmE=");
    EXPECT_EQ(base64Encode("foobar"), "Zm9vYmFy");
    for (std::string s : {"", "f", "fo", "foo", "foob", "fooba", "foobar"}) {
        auto d = base64Decode(base64Encode(s));
        ASSERT_TRUE(d.has_value());
        EXPECT_EQ(*d, s);
    }
}

TEST(Base64, DecodeIsTolerantAndStrict) {
    EXPECT_EQ(base64Decode("Zm9v\nYmFy").value(), "foobar");
    EXPECT_EQ(base64Decode("Zm9vYg").value(), "foob");  // missing padding
    EXPECT_FALSE(base64Decode("Zm9v*mFy").has_value());
    EXPECT_FALSE(base64Decode("Zg==Zg").has_value());
    std::string binary("\x00\xff\x10\x80", 4);
    EXPECT_EQ(base64Decode(base64Encode(binary)).value(), binary);
}

TEST(WsSecurity, PasswordDigestMatchesOasisExample) {
    // OASIS WS-Security UsernameToken Profile 1.1 example:
    // Nonce LKqI6G/AikKCQrN0zqZFlg==, Created 2010-09-16T07:50:45Z, password "userpassword"
    // -> tuOSpGlFlIXsozq4HFNeeGeFLEI= (also cross-checked with Python hashlib).
    const auto nonce = base64Decode("LKqI6G/AikKCQrN0zqZFlg==");
    ASSERT_TRUE(nonce.has_value());
    EXPECT_EQ(passwordDigest(*nonce, "2010-09-16T07:50:45Z", "userpassword"), "tuOSpGlFlIXsozq4HFNeeGeFLEI=");
}

TEST(TimeUtil, FormatAndParseRoundTrip) {
    EXPECT_EQ(formatIso8601Utc(0), "1970-01-01T00:00:00Z");
    EXPECT_EQ(formatIso8601Utc(1284623445), "2010-09-16T07:50:45Z");
    EXPECT_EQ(formatIso8601Utc(951782400), "2000-02-29T00:00:00Z");  // leap day
    EXPECT_EQ(parseIso8601("2010-09-16T07:50:45Z").value(), 1284623445);
    EXPECT_EQ(parseIso8601("2010-09-16T07:50:45.123Z").value(), 1284623445);
    EXPECT_EQ(parseIso8601("2010-09-16T09:50:45+02:00").value(), 1284623445);
    EXPECT_EQ(parseIso8601("2010-09-16T05:20:45-02:30").value(), 1284623445);
    for (std::int64_t t : {0LL, 86399LL, 1700000000LL, 4102444800LL})
        EXPECT_EQ(parseIso8601(formatIso8601Utc(t)).value(), t);
}

TEST(TimeUtil, RejectsInvalid) {
    EXPECT_FALSE(parseIso8601("").has_value());
    EXPECT_FALSE(parseIso8601("garbage").has_value());
    EXPECT_FALSE(parseIso8601("2010-13-16T07:50:45Z").has_value());
    EXPECT_FALSE(civilToUnix(2020, 1, 1, 24, 0, 0).has_value());
}

TEST(Url, ParsesVariants) {
    auto u = parseUrl("http://192.168.1.64/onvif/device_service");
    ASSERT_TRUE(u.ok());
    EXPECT_EQ(u.value().host, "192.168.1.64");
    EXPECT_EQ(u.value().port, 80);
    EXPECT_EQ(u.value().path, "/onvif/device_service");

    u = parseUrl("HTTPS://cam.local:8443/x?y=1#f");
    ASSERT_TRUE(u.ok());
    EXPECT_EQ(u.value().scheme, "https");
    EXPECT_EQ(u.value().port, 8443);
    EXPECT_EQ(u.value().path, "/x?y=1");

    u = parseUrl("rtsp://admin:s3cret@10.0.0.5/stream1");
    ASSERT_TRUE(u.ok());
    EXPECT_EQ(u.value().port, 554);
    EXPECT_EQ(u.value().userinfo, "admin:s3cret");
    EXPECT_EQ(urlToString(u.value()).find("s3cret"), std::string::npos);  // safe to log

    u = parseUrl("http://[fe80::1]:8080");
    ASSERT_TRUE(u.ok());
    EXPECT_EQ(u.value().host, "fe80::1");
    EXPECT_EQ(u.value().path, "/");
}

TEST(Url, RejectsBadInput) {
    EXPECT_FALSE(parseUrl("").ok());
    EXPECT_FALSE(parseUrl("no-scheme").ok());
    EXPECT_FALSE(parseUrl("http://").ok());
    EXPECT_FALSE(parseUrl("http://h:99999/").ok());
    EXPECT_FALSE(parseUrl("http://h:abc/").ok());
    EXPECT_FALSE(parseUrl("http://[::1/").ok());
}

TEST(Url, ReplaceHost) {
    EXPECT_EQ(replaceUrlHost("http://0.0.0.0:8080/onvif/media", "192.168.1.9"), "http://192.168.1.9:8080/onvif/media");
    EXPECT_EQ(replaceUrlHost("rtsp://u:p@127.0.0.1/a?b=1", "10.0.0.2"), "rtsp://u:p@10.0.0.2/a?b=1");
    EXPECT_EQ(replaceUrlHost("http://h", "x"), "http://x");
    EXPECT_EQ(replaceUrlHost("garbage", "x"), "garbage");
    EXPECT_TRUE(isUnroutableHost("0.0.0.0"));
    EXPECT_TRUE(isUnroutableHost("127.0.1.1"));
    EXPECT_FALSE(isUnroutableHost("192.168.1.10"));
}
