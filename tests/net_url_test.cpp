#include <gtest/gtest.h>

#include "net/url.hpp"

using namespace hb::net;

TEST(NetUrl, ParsesBasicHttps) {
    Url u;
    ASSERT_TRUE(parseUrl("https://Example.com/a/b?x=1#frag", u));
    EXPECT_EQ(u.scheme, "https");
    EXPECT_EQ(u.host, "example.com");
    EXPECT_EQ(u.port, 443);
    EXPECT_EQ(u.target, "/a/b?x=1");
}

TEST(NetUrl, DefaultsAndExplicitPort) {
    Url u;
    ASSERT_TRUE(parseUrl("http://192.168.0.5:8080", u));
    EXPECT_EQ(u.port, 8080);
    EXPECT_EQ(u.target, "/");
    ASSERT_TRUE(parseUrl("http://host?q=1", u));
    EXPECT_EQ(u.port, 80);
    EXPECT_EQ(u.target, "/?q=1");
}

TEST(NetUrl, Ipv6Literal) {
    Url u;
    ASSERT_TRUE(parseUrl("http://[::1]:11434/v1", u));
    EXPECT_EQ(u.host, "::1");
    EXPECT_EQ(u.port, 11434);
    EXPECT_FALSE(parseUrl("http://[::1", u));
    EXPECT_FALSE(parseUrl("http://[::1]x", u));
}

TEST(NetUrl, RejectsMalformed) {
    Url u;
    std::string why;
    EXPECT_FALSE(parseUrl("ftp://host/x", u, &why));
    EXPECT_FALSE(parseUrl("https://", u));
    EXPECT_FALSE(parseUrl("https://user:pw@host/", u));
    EXPECT_FALSE(parseUrl("https://host:0/", u));
    EXPECT_FALSE(parseUrl("https://host:70000/", u));
    EXPECT_FALSE(parseUrl("https://host:/", u));
    EXPECT_FALSE(parseUrl("https://host:12ab/", u));
    EXPECT_FALSE(parseUrl("https://ho st/", u));
    EXPECT_FALSE(parseUrl("https://host\\evil/", u));
    EXPECT_FALSE(parseUrl("https://host/\r\nX: y", u));
    EXPECT_FALSE(parseUrl("https://ho$t/", u));
}

TEST(NetUrl, StrictLoopback) {
    EXPECT_TRUE(isLoopbackHost("localhost"));
    EXPECT_TRUE(isLoopbackHost("LOCALHOST"));
    EXPECT_TRUE(isLoopbackHost("127.0.0.1"));
    EXPECT_TRUE(isLoopbackHost("127.255.0.3"));
    EXPECT_TRUE(isLoopbackHost("::1"));
    EXPECT_TRUE(isLoopbackHost("[::1]"));
    // The historical bug: a hostname merely starting with "127." must not be loopback.
    EXPECT_FALSE(isLoopbackHost("127.evil.com"));
    EXPECT_FALSE(isLoopbackHost("127.0.0.1.evil.com"));
    EXPECT_FALSE(isLoopbackHost("127.0.0"));
    EXPECT_FALSE(isLoopbackHost("127.0.0.1.5"));
    EXPECT_FALSE(isLoopbackHost("127.0.0.256"));
    EXPECT_FALSE(isLoopbackHost("localhost.evil.com"));
    EXPECT_FALSE(isLoopbackHost("128.0.0.1"));
    EXPECT_FALSE(isLoopbackHost(""));
}

TEST(NetUrl, Rfc1918) {
    EXPECT_TRUE(isRfc1918Host("10.0.0.1"));
    EXPECT_TRUE(isRfc1918Host("192.168.1.20"));
    EXPECT_TRUE(isRfc1918Host("172.16.0.1"));
    EXPECT_TRUE(isRfc1918Host("172.31.255.255"));
    EXPECT_FALSE(isRfc1918Host("172.15.0.1"));
    EXPECT_FALSE(isRfc1918Host("172.32.0.1"));
    EXPECT_FALSE(isRfc1918Host("192.169.0.1"));
    EXPECT_FALSE(isRfc1918Host("10.evil.com"));
    EXPECT_FALSE(isRfc1918Host("10.0.0.1.evil.com"));
    EXPECT_FALSE(isRfc1918Host("11.0.0.1"));
}

TEST(NetUrl, LeadingZerosAreNotDottedQuads) {
    int o[4];
    EXPECT_FALSE(parseDottedQuad("010.0.0.1", o));  // octal ambiguity
    EXPECT_FALSE(isRfc1918Host("010.1.1.1"));
    EXPECT_FALSE(isLoopbackHost("0127.0.0.1"));
    EXPECT_TRUE(parseDottedQuad("0.0.0.0", o));
    EXPECT_TRUE(parseDottedQuad("100.20.30.40", o));
    EXPECT_EQ(o[0], 100);
    EXPECT_EQ(o[3], 40);
}

TEST(NetUrl, LocalNetworkHost) {
    EXPECT_TRUE(isLocalNetworkHost("localhost"));
    EXPECT_TRUE(isLocalNetworkHost("nas"));
    EXPECT_TRUE(isLocalNetworkHost("minio.local"));
    EXPECT_TRUE(isLocalNetworkHost("box.lan"));
    EXPECT_TRUE(isLocalNetworkHost("10.1.2.3"));
    EXPECT_TRUE(isLocalNetworkHost("127.0.0.1"));
    EXPECT_FALSE(isLocalNetworkHost("example.com"));
    EXPECT_FALSE(isLocalNetworkHost("8.8.8.8"));
    EXPECT_FALSE(isLocalNetworkHost("127.evil.com"));
    EXPECT_FALSE(isLocalNetworkHost("10.0.0.1.evil.com"));
    EXPECT_FALSE(isLocalNetworkHost("local.example.com"));
    EXPECT_FALSE(isLocalNetworkHost(""));
}
