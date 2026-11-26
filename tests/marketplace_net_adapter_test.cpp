#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>

#include "marketplace/net_adapter.hpp"
#include "net/http_fake.hpp"

using namespace hb;

namespace {
std::string tmpFile(const char* name) { return (std::filesystem::path(::testing::TempDir()) / name).string(); }
std::string slurp(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
}  // namespace

TEST(MarketplaceNetAdapter, FetchReturnsBodyAndEtagFor200Only) {
    auto fake = std::make_shared<net::FakeHttpClient>();
    net::FakeHttpClient::Reply ok;
    ok.headers = {{"ETag", "\"abc\""}};
    ok.body = "{\"plugins\":[]}";
    net::FakeHttpClient::Reply nm;
    nm.status = 304;
    nm.headers = {{"ETag", "\"abc\""}};
    fake->enqueue(ok);
    fake->enqueue(nm);
    auto dl = marketplace::makeDownloaderOverNet(fake);
    marketplace::FetchRequest rq;
    rq.url = "https://example.com/index.json";
    rq.ifNoneMatch = "\"abc\"\r\nX-Injected: 1";
    auto a = dl->fetch(rq);
    EXPECT_TRUE(a.transportOk);
    EXPECT_EQ(a.status, 200);
    EXPECT_EQ(a.body, "{\"plugins\":[]}");
    EXPECT_EQ(a.etag, "\"abc\"");
    // No CR/LF can survive into the conditional header.
    bool found = false;
    const auto sent = fake->requests();   // keep the copy alive: iterating a member of a temporary dangles (passed on Linux by luck, failed on MSVC)
    for (const auto& h : sent.at(0).headers)
        if (h.first == "If-None-Match") {
            found = true;
            EXPECT_EQ(h.second.find('\r'), std::string::npos);
        }
    EXPECT_TRUE(found);
    auto b = dl->fetch(rq);
    EXPECT_TRUE(b.transportOk);
    EXPECT_EQ(b.status, 304);
    EXPECT_TRUE(b.body.empty());
}

TEST(MarketplaceNetAdapter, FetchRefusesUnsafeUrlsAndOversizedBodies) {
    auto fake = std::make_shared<net::FakeHttpClient>();
    net::FakeHttpClient::Reply big;
    big.body = std::string(100, 'x');
    big.chunk = 10;
    fake->enqueue(big);
    auto dl = marketplace::makeDownloaderOverNet(fake);
    marketplace::FetchRequest rq;
    rq.url = "http://example.com/index.json";
    EXPECT_FALSE(dl->fetch(rq).transportOk);
    EXPECT_EQ(fake->requestCount(), 0u);
    rq.url = "https://example.com/index.json";
    rq.maxBytes = 50;
    auto r = dl->fetch(rq);
    EXPECT_FALSE(r.transportOk);
    EXPECT_EQ(r.error, "resposta grande demais");
}

TEST(MarketplaceNetAdapter, FetchMapsFailures) {
    auto fake = std::make_shared<net::FakeHttpClient>();
    net::FakeHttpClient::Reply tls;
    tls.error = net::ErrorKind::Transport;
    tls.failure = net::Failure::Tls;
    fake->enqueue(tls);
    auto dl = marketplace::makeDownloaderOverNet(fake);
    marketplace::FetchRequest rq;
    rq.url = "https://example.com/index.json";
    auto r = dl->fetch(rq);
    EXPECT_FALSE(r.transportOk);
    EXPECT_EQ(r.error, "falha na verificação do certificado HTTPS");
}

TEST(MarketplaceNetAdapter, DownloadWritesPartThenRenames) {
    auto fake = std::make_shared<net::FakeHttpClient>();
    net::FakeHttpClient::Reply ok;
    ok.headers = {{"Content-Length", "6"}};
    ok.body = "abcdef";
    ok.chunk = 3;
    fake->enqueue(ok);
    auto dl = marketplace::makeDownloaderOverNet(fake);
    marketplace::FileRequest rq;
    rq.url = "https://example.com/p.zip";
    rq.destPathUtf8 = tmpFile("mk_dl_ok.zip");
    rq.maxBytes = 100;
    std::uint64_t last = 0;
    auto r = dl->downloadToFile(rq, [&](std::uint64_t got, std::uint64_t) {
        last = got;
        return true;
    });
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(r.bytes, 6u);
    EXPECT_EQ(last, 6u);
    EXPECT_EQ(slurp(rq.destPathUtf8), "abcdef");
    EXPECT_FALSE(std::filesystem::exists(rq.destPathUtf8 + ".part"));
    std::filesystem::remove(rq.destPathUtf8);
}

TEST(MarketplaceNetAdapter, DownloadFailuresLeaveNoPartFile) {
    auto fake = std::make_shared<net::FakeHttpClient>();
    net::FakeHttpClient::Reply declaredBig;
    declaredBig.headers = {{"Content-Length", "1000"}};
    declaredBig.body = "x";
    net::FakeHttpClient::Reply streamedBig;
    streamedBig.body = std::string(60, 'y');
    streamedBig.chunk = 20;
    net::FakeHttpClient::Reply nf;
    nf.status = 404;
    net::FakeHttpClient::Reply ok;
    ok.body = "123456";
    ok.chunk = 2;
    fake->enqueue(declaredBig);
    fake->enqueue(streamedBig);
    fake->enqueue(nf);
    fake->enqueue(ok);
    auto dl = marketplace::makeDownloaderOverNet(fake);
    marketplace::FileRequest rq;
    rq.url = "https://example.com/p.zip";
    rq.destPathUtf8 = tmpFile("mk_dl_bad.zip");
    rq.maxBytes = 50;
    auto a = dl->downloadToFile(rq, nullptr);
    EXPECT_FALSE(a.ok);
    EXPECT_EQ(a.error, "o arquivo é maior que o tamanho declarado no catálogo");
    auto b = dl->downloadToFile(rq, nullptr);
    EXPECT_FALSE(b.ok);
    EXPECT_EQ(b.error, "o arquivo é maior que o tamanho declarado no catálogo");
    auto c = dl->downloadToFile(rq, nullptr);
    EXPECT_FALSE(c.ok);
    EXPECT_EQ(c.status, 404);
    EXPECT_EQ(c.error, "o servidor respondeu HTTP 404");
    auto d = dl->downloadToFile(rq, [](std::uint64_t, std::uint64_t) { return false; });
    EXPECT_FALSE(d.ok);
    EXPECT_TRUE(d.cancelled);
    EXPECT_FALSE(std::filesystem::exists(rq.destPathUtf8 + ".part"));
    EXPECT_FALSE(std::filesystem::exists(rq.destPathUtf8));
}
