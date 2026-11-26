#include <gtest/gtest.h>

#include "chroma/net_adapter.hpp"
#include "net/http_fake.hpp"

using namespace hb;

TEST(ChromaNetAdapter, StreamsBodyWithTotal) {
    auto fake = std::make_shared<net::FakeHttpClient>();
    net::FakeHttpClient::Reply rep;
    rep.headers = {{"Content-Length", "6"}};
    rep.body = "abcdef";
    rep.chunk = 3;
    fake->enqueue(rep);
    auto http = chroma::makeChromaHttpOverNet(fake);
    std::string got;
    std::uint64_t total = 0;
    const auto r = http->get("https://huggingface.co/m.onnx", [&](const std::uint8_t* d, std::size_t n, std::uint64_t t) {
        got.append(reinterpret_cast<const char*>(d), n);
        total = t;
        return true;
    });
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(got, "abcdef");
    EXPECT_EQ(total, 6u);
}

TEST(ChromaNetAdapter, RefusesPlainHttpWithoutTouchingTheNetwork) {
    auto fake = std::make_shared<net::FakeHttpClient>();
    auto http = chroma::makeChromaHttpOverNet(fake);
    const auto r = http->get("http://example.com/m.onnx", [](const std::uint8_t*, std::size_t, std::uint64_t) { return true; });
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error, "Somente HTTPS é permitido");
    EXPECT_EQ(fake->requestCount(), 0u);
}

TEST(ChromaNetAdapter, NonOkStatusAndAbort) {
    auto fake = std::make_shared<net::FakeHttpClient>();
    net::FakeHttpClient::Reply nf;
    nf.status = 404;
    nf.body = "page";
    fake->enqueue(nf);
    net::FakeHttpClient::Reply ok;
    ok.body = "123456";
    ok.chunk = 2;
    fake->enqueue(ok);
    auto http = chroma::makeChromaHttpOverNet(fake);
    int calls = 0;
    auto sink = [&](const std::uint8_t*, std::size_t, std::uint64_t) {
        ++calls;
        return calls < 2;
    };
    auto a = http->get("https://x.example/a", sink);
    EXPECT_FALSE(a.ok);
    EXPECT_EQ(a.status, 404);
    EXPECT_EQ(a.error, "HTTP 404");
    EXPECT_EQ(calls, 0);  // error bodies are never fed to the sink
    auto b = http->get("https://x.example/a", sink);
    EXPECT_FALSE(b.ok);
    EXPECT_EQ(b.error, "Interrompido");
}
