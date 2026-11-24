#include <gtest/gtest.h>

#include <atomic>
#include <thread>

#include "chat/loopback.hpp"
#include "chat/oauth.hpp"
#include "chat_test_util.hpp"

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

using namespace hb::chat;
using namespace hb::chat::test;

TEST(ChatOAuth, PkceMatchesRfc7636Example) {
    EXPECT_EQ(pkceChallengeS256("dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk"), "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM");
}

TEST(ChatOAuth, EncodingHelpers) {
    EXPECT_EQ(urlEncode("a b&c=d/é"), "a%20b%26c%3Dd%2F%C3%A9");
    EXPECT_EQ(urlDecode("a%20b+c%2"), "a b c%2");
    EXPECT_EQ(base64Url(std::string("\xfb\xff\xfe", 3)), "-__-");
    EXPECT_EQ(base64Url("a"), "YQ");
}

TEST(ChatOAuth, AuthRequestHasPkceStateAndLoopback) {
    unsigned char counter = 0;
    RandomFn rnd = [&](unsigned char* p, std::size_t n) { for (std::size_t i = 0; i < n; ++i) p[i] = counter++; };
    const AuthRequest r = buildGoogleAuthRequest("cid.apps.googleusercontent.com", "http://127.0.0.1:5555", rnd);
    EXPECT_GE(r.verifier.size(), 43u);
    EXPECT_LE(r.verifier.size(), 128u);
    EXPECT_NE(r.verifier, r.state);
    EXPECT_EQ(r.url.rfind("https://accounts.google.com/o/oauth2/v2/auth?", 0), 0u);
    EXPECT_NE(r.url.find("code_challenge=" + pkceChallengeS256(r.verifier)), std::string::npos);
    EXPECT_NE(r.url.find("code_challenge_method=S256"), std::string::npos);
    EXPECT_NE(r.url.find("redirect_uri=http%3A%2F%2F127.0.0.1%3A5555"), std::string::npos);
    EXPECT_NE(r.url.find("scope=https%3A%2F%2Fwww.googleapis.com%2Fauth%2Fyoutube.readonly"), std::string::npos);
    EXPECT_NE(r.url.find("state=" + r.state), std::string::npos);
    EXPECT_NE(r.url.find("access_type=offline"), std::string::npos);
    EXPECT_EQ(r.url.find(r.verifier), std::string::npos);   // the verifier itself is never sent to the browser
}

TEST(ChatOAuth, ParsesLoopbackRequests) {
    auto r = parseLoopbackRequest("GET /?state=S%201&code=4%2FABC&scope=x HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.code, "4/ABC");
    EXPECT_EQ(r.state, "S 1");
    r = parseLoopbackRequest("GET /?error=access_denied&state=s HTTP/1.1\r\n\r\n");
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.error, "access_denied");
    EXPECT_FALSE(parseLoopbackRequest("GET /favicon.ico HTTP/1.1\r\n\r\n").ok);
    EXPECT_FALSE(parseLoopbackRequest("POST /?code=x HTTP/1.1\r\n\r\n").ok);
    EXPECT_FALSE(parseLoopbackRequest("").ok);
}

TEST(ChatOAuth, TokenResponseParsing) {
    TokenSet t;
    std::string err;
    ASSERT_TRUE(parseTokenResponse(R"({"access_token":"AT","expires_in":3599,"refresh_token":"RT","token_type":"Bearer"})", t, err));
    EXPECT_EQ(t.accessToken, "AT");
    EXPECT_EQ(t.refreshToken, "RT");
    EXPECT_EQ(t.expiresInSec, 3599);
    EXPECT_FALSE(parseTokenResponse(R"({"error":"invalid_grant","error_description":"Bad"})", t, err));
    EXPECT_EQ(err, "invalid_grant");
    EXPECT_FALSE(parseTokenResponse("nope", t, err));
    EXPECT_FALSE(parseTokenResponse("{}", t, err));
}

TEST(ChatOAuth, TokenManagerExchangeRefreshAndCaching) {
    FakeHttp http;
    MemorySecretStore secrets;
    GoogleTokenManager tm(http, secrets);
    tm.setClient("cid", "csec");
    std::string tok, err;
    bool needs = false;
    EXPECT_FALSE(tm.accessToken(at(0), tok, err, needs));   // nothing authorized yet
    EXPECT_TRUE(needs);
    http.ok(200, R"({"access_token":"AT1","expires_in":3600,"refresh_token":"RT1"})");
    ASSERT_TRUE(tm.exchangeCode("CODE", "VERIFIER", "http://127.0.0.1:1", at(0), err));
    ASSERT_EQ(http.requests.size(), 1u);
    const auto& b = http.requests[0].body;
    EXPECT_NE(b.find("grant_type=authorization_code"), std::string::npos);
    EXPECT_NE(b.find("code_verifier=VERIFIER"), std::string::npos);
    EXPECT_NE(b.find("client_secret=csec"), std::string::npos);
    EXPECT_TRUE(tm.hasRefreshToken());
    ASSERT_TRUE(tm.accessToken(at(1000), tok, err, needs));
    EXPECT_EQ(tok, "AT1");
    EXPECT_EQ(http.requests.size(), 1u);   // cached
    // near expiry -> refresh (no refresh_token in the response keeps the stored one)
    http.ok(200, R"({"access_token":"AT2","expires_in":3600})");
    ASSERT_TRUE(tm.accessToken(at(3560 * 1000), tok, err, needs));
    EXPECT_EQ(tok, "AT2");
    EXPECT_NE(http.requests[1].body.find("refresh_token=RT1"), std::string::npos);
    std::string stored;
    ASSERT_TRUE(secrets.get(kYouTubeRefreshKey, stored));
    EXPECT_EQ(stored, "RT1");
    // 401 handling: invalidate forces a refresh
    http.ok(200, R"({"access_token":"AT3","expires_in":3600})");
    tm.invalidate();
    ASSERT_TRUE(tm.accessToken(at(3561 * 1000), tok, err, needs));
    EXPECT_EQ(tok, "AT3");
}

TEST(ChatOAuth, RevokedGrantNeedsReauthAndForgetsToken) {
    FakeHttp http;
    MemorySecretStore secrets;
    secrets.put(kYouTubeRefreshKey, "RT");
    GoogleTokenManager tm(http, secrets);
    tm.setClient("cid", "");
    http.ok(400, R"({"error":"invalid_grant"})");
    std::string tok, err;
    bool needs = false;
    EXPECT_FALSE(tm.accessToken(at(0), tok, err, needs));
    EXPECT_TRUE(needs);
    EXPECT_FALSE(tm.hasRefreshToken());
    EXPECT_EQ(http.requests[0].body.find("client_secret"), std::string::npos);
    // a network failure is transient, not a reauth request
    secrets.put(kYouTubeRefreshKey, "RT");
    http.netFail("timeout");
    EXPECT_FALSE(tm.accessToken(at(1), tok, err, needs));
    EXPECT_FALSE(needs);
    EXPECT_TRUE(tm.hasRefreshToken());
}

TEST(ChatOAuth, ExchangeWithoutRefreshTokenFails) {
    FakeHttp http;
    MemorySecretStore secrets;
    GoogleTokenManager tm(http, secrets);
    tm.setClient("cid", "");
    http.ok(200, R"({"access_token":"AT","expires_in":10})");
    std::string err;
    EXPECT_FALSE(tm.exchangeCode("c", "v", "http://127.0.0.1:1", at(0), err));
    EXPECT_FALSE(tm.hasRefreshToken());
}

#ifndef _WIN32
namespace {
std::string clientGet(int port, const std::string& target) {
    int s = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(static_cast<unsigned short>(port));
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(s, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) { ::close(s); return ""; }
    const std::string req = "GET " + target + " HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
    ::send(s, req.data(), req.size(), 0);
    std::string resp;
    char buf[512];
    ssize_t n;
    while ((n = ::recv(s, buf, sizeof buf, 0)) > 0) resp.append(buf, static_cast<std::size_t>(n));
    ::close(s);
    return resp;
}
}  // namespace

TEST(ChatLoopback, ReceivesRedirectIgnoringOtherRequests) {
    LoopbackServer srv;
    ASSERT_TRUE(srv.listen());
    ASSERT_GT(srv.port(), 0);
    std::string favicon, ok;
    std::thread client([&] {
        favicon = clientGet(srv.port(), "/favicon.ico");
        ok = clientGet(srv.port(), "/?code=THECODE&state=ST");
    });
    const std::string raw = srv.waitForRedirect(5000, "<html>done</html>", nullptr);
    client.join();
    const auto r = parseLoopbackRequest(raw);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.code, "THECODE");
    EXPECT_EQ(r.state, "ST");
    EXPECT_NE(favicon.find("404"), std::string::npos);
    EXPECT_NE(ok.find("200 OK"), std::string::npos);
    EXPECT_NE(ok.find("done"), std::string::npos);
}

TEST(ChatLoopback, CancelReturnsPromptly) {
    LoopbackServer srv;
    ASSERT_TRUE(srv.listen());
    std::atomic<bool> cancel{false};
    std::thread t([&] { std::this_thread::sleep_for(std::chrono::milliseconds(50)); cancel = true; });
    const auto start = std::chrono::steady_clock::now();
    EXPECT_EQ(srv.waitForRedirect(10000, "x", &cancel), "");
    t.join();
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(3));
}
#endif
