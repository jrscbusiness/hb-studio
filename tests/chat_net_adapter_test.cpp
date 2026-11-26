#include <gtest/gtest.h>

#include <filesystem>

#include "chat/net_adapter.hpp"
#include "net/http_fake.hpp"

using namespace hb;

TEST(ChatNetAdapter, HttpStatusIsNotATransportFailure) {
    auto fake = std::make_shared<net::FakeHttpClient>();
    net::FakeHttpClient::Reply rep;
    rep.status = 403;
    rep.body = "{\"error\":\"quota\"}";
    fake->enqueue(rep);
    auto http = chat::makeChatHttpOverNet(fake);
    chat::HttpRequest r;
    r.url = "https://www.googleapis.com/youtube/v3/liveChat/messages";
    r.timeoutMs = 10;  // clamped up to 1 s
    const auto resp = http->send(r);
    EXPECT_TRUE(resp.transportOk);
    EXPECT_EQ(resp.status, 403);
    EXPECT_EQ(resp.body, "{\"error\":\"quota\"}");
    EXPECT_EQ(fake->requests().at(0).timeouts.connectMs, 1000);
}

TEST(ChatNetAdapter, FailuresAreTransportErrors) {
    auto fake = std::make_shared<net::FakeHttpClient>();
    net::FakeHttpClient::Reply big;
    big.body = std::string((4u << 20) + 1, 'x');
    big.chunk = 1 << 20;
    fake->enqueue(big);
    auto http = chat::makeChatHttpOverNet(fake);
    chat::HttpRequest r;
    r.url = "https://x.example.com/";
    auto a = http->send(r);
    EXPECT_FALSE(a.transportOk);
    EXPECT_EQ(a.error, "response too large");
    r.url = "nonsense";
    auto b = http->send(r);
    EXPECT_FALSE(b.transportOk);
    EXPECT_EQ(b.error, "invalid URL");
}

TEST(ChatNetAdapter, FileSecretStoreOneFilePerKey) {
    const auto dir = std::filesystem::path(::testing::TempDir()) / "chat_secrets_net";
    std::filesystem::remove_all(dir);
    auto prot = std::make_shared<net::FakeSecretStore>();
    auto s = chat::makeFileSecretStore(dir.string(), prot);
    ASSERT_TRUE(s->put("google.refresh", "tok-123"));
    ASSERT_TRUE(s->put("weird key/../x", "v2"));  // sanitised file name, stays inside dir
    std::string out;
    ASSERT_TRUE(s->get("google.refresh", out));
    EXPECT_EQ(out, "tok-123");
    EXPECT_TRUE(std::filesystem::exists(dir / "google.refresh.bin"));
    EXPECT_FALSE(s->get("absent", out));
    EXPECT_FALSE(s->put("", "x"));
    EXPECT_FALSE(s->put(std::string(65, 'k'), "x"));
    EXPECT_TRUE(s->put("google.refresh", "tok-456"));  // overwrite
    ASSERT_TRUE(s->get("google.refresh", out));
    EXPECT_EQ(out, "tok-456");
    EXPECT_TRUE(s->erase("google.refresh"));
    EXPECT_FALSE(s->erase("google.refresh"));
    prot->failUnprotect = true;
    EXPECT_FALSE(s->get("weird key/../x", out));
    std::filesystem::remove_all(dir);
}
