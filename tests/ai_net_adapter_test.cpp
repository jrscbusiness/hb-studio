#include <gtest/gtest.h>

#include "ai/net_adapter.hpp"
#include "net/http_fake.hpp"

using namespace hb;

namespace {
ai::HttpRequest req(const std::string& url = "https://api.example.com/v1/chat") {
    ai::HttpRequest r;
    r.url = url;
    r.body = "{}";
    r.headers = {{"Authorization", "Bearer k"}};
    return r;
}
}  // namespace

TEST(AiNetAdapter, MapsStatusBodyAndRetryAfterOnly) {
    auto fake = std::make_shared<net::FakeHttpClient>();
    net::FakeHttpClient::Reply rep;
    rep.status = 429;
    rep.headers = {{"retry-after", "9"}, {"X-Other", "1"}};
    rep.body = "slow down";
    fake->enqueue(rep);
    auto http = ai::makeAiHttpOverNet(fake);
    const auto r = http->send(req());
    EXPECT_EQ(r.status, 429);
    EXPECT_EQ(r.body, "slow down");
    ASSERT_EQ(r.headers.size(), 1u);
    EXPECT_EQ(r.headers[0].first, "Retry-After");
    EXPECT_EQ(r.headers[0].second, "9");
    EXPECT_TRUE(r.transportError.empty());
    const auto sent = fake->requests().at(0);
    EXPECT_EQ(sent.body, "{}");
    EXPECT_EQ(sent.headers.at(0).second, "Bearer k");
    EXPECT_GE(sent.timeouts.receiveMs, 1000);
}

TEST(AiNetAdapter, StreamsOnlySuccessfulResponses) {
    auto fake = std::make_shared<net::FakeHttpClient>();
    net::FakeHttpClient::Reply ok;
    ok.body = "data: a\n\ndata: b\n\n";
    ok.chunk = 9;
    net::FakeHttpClient::Reply bad;
    bad.status = 401;
    bad.body = "{\"error\":1}";
    fake->enqueue(ok);
    fake->enqueue(bad);
    auto http = ai::makeAiHttpOverNet(fake);
    std::string seen;
    auto r = req();
    r.onData = [&](const std::string& c) {
        seen += c;
        return true;
    };
    auto a = http->send(r);
    EXPECT_EQ(a.status, 200);
    EXPECT_EQ(seen, "data: a\n\ndata: b\n\n");
    EXPECT_TRUE(a.body.empty());
    seen.clear();
    auto b = http->send(r);
    EXPECT_EQ(b.status, 401);
    EXPECT_EQ(b.body, "{\"error\":1}");
    EXPECT_TRUE(seen.empty());
}

TEST(AiNetAdapter, ConsumerAbortIsNotAnError) {
    auto fake = std::make_shared<net::FakeHttpClient>();
    net::FakeHttpClient::Reply ok;
    ok.body = "1234567890";
    ok.chunk = 2;
    fake->enqueue(ok);
    auto http = ai::makeAiHttpOverNet(fake);
    int n = 0;
    auto r = req();
    r.onData = [&](const std::string&) { return ++n < 2; };
    const auto resp = http->send(r);
    EXPECT_EQ(resp.status, 200);
    EXPECT_TRUE(resp.transportError.empty());
    EXPECT_EQ(n, 2);
}

TEST(AiNetAdapter, OversizedBodyAndTransportErrors) {
    auto fake = std::make_shared<net::FakeHttpClient>();
    net::FakeHttpClient::Reply big;
    big.body = std::string(ai::kMaxAiResponseBytes + 1, 'x');
    big.chunk = 1 << 20;
    fake->enqueue(big);
    net::FakeHttpClient::Reply to;
    to.error = net::ErrorKind::Timeout;
    fake->enqueue(to);
    auto http = ai::makeAiHttpOverNet(fake);
    auto a = http->send(req());
    EXPECT_EQ(a.status, 0);
    EXPECT_EQ(a.transportError, "resposta grande demais");
    auto b = http->send(req());
    EXPECT_EQ(b.status, 0);
    EXPECT_TRUE(b.timedOut);
    EXPECT_FALSE(b.transportError.empty());
}

TEST(AiNetAdapter, BadUrlIsRefusedBeforeTheTransport) {
    auto fake = std::make_shared<net::FakeHttpClient>();
    auto http = ai::makeAiHttpOverNet(fake);
    const auto r = http->send(req("not a url"));
    EXPECT_EQ(r.status, 0);
    EXPECT_EQ(r.transportError, "endereço inválido");
    EXPECT_EQ(fake->requestCount(), 0u);
}
