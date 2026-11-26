#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>

#include "captions/net_adapter.hpp"
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

TEST(CaptionsNetAdapter, DownloadWritesFileAndReportsProgress) {
    auto fake = std::make_shared<net::FakeHttpClient>();
    net::FakeHttpClient::Reply rep;
    rep.headers = {{"Content-Length", "8"}};
    rep.body = "12345678";
    rep.chunk = 4;
    fake->enqueue(rep);
    captions::NetBackedHttp http(fake);
    std::uint64_t lastGot = 0, lastTotal = 0;
    std::string err;
    const std::string path = tmpFile("cap_dl_ok.bin");
    ASSERT_TRUE(http.download("https://huggingface.co/m.bin", path, [&](std::uint64_t g, std::uint64_t t) {
        lastGot = g;
        lastTotal = t;
        return true;
    }, err)) << err;
    EXPECT_EQ(slurp(path), "12345678");
    EXPECT_EQ(lastGot, 8u);
    EXPECT_EQ(lastTotal, 8u);
    std::filesystem::remove(path);
}

TEST(CaptionsNetAdapter, DownloadFailureModes) {
    auto fake = std::make_shared<net::FakeHttpClient>();
    net::FakeHttpClient::Reply nf;
    nf.status = 404;
    fake->enqueue(nf);
    net::FakeHttpClient::Reply cut;  // Content-Length says 10, only 4 bytes arrive
    cut.headers = {{"Content-Length", "10"}};
    cut.body = "1234";
    fake->enqueue(cut);
    net::FakeHttpClient::Reply ok;
    ok.body = "abcdef";
    ok.chunk = 2;
    fake->enqueue(ok);
    captions::NetBackedHttp http(fake);
    std::string err;
    const std::string path = tmpFile("cap_dl_bad.bin");
    EXPECT_FALSE(http.download("http://insecure.example/m.bin", path, nullptr, err));
    EXPECT_EQ(err, "endereço de download inválido");
    EXPECT_EQ(fake->requestCount(), 0u);
    EXPECT_FALSE(http.download("https://x.example/m.bin", path, nullptr, err));
    EXPECT_EQ(err, "servidor respondeu HTTP 404");
    EXPECT_FALSE(http.download("https://x.example/m.bin", path, nullptr, err));
    EXPECT_EQ(err, "download incompleto");
    EXPECT_FALSE(http.download("https://x.example/m.bin", path, [](std::uint64_t, std::uint64_t) { return false; }, err));
    EXPECT_EQ(err, "cancelado");
    std::filesystem::remove(path);
}

TEST(CaptionsNetAdapter, PostPassesBodyHeadersAndCapsReply) {
    auto fake = std::make_shared<net::FakeHttpClient>();
    net::FakeHttpClient::Reply rep;
    rep.status = 200;
    rep.body = "{\"text\":\"ola\"}";
    fake->enqueue(rep);
    net::FakeHttpClient::Reply big;
    big.body = std::string((4u << 20) + 10, 'x');
    big.chunk = 1 << 20;
    fake->enqueue(big);
    captions::NetBackedHttp http(fake);
    auto r = http.post("https://api.example.com/v1/audio", {{"Authorization", "Bearer k"}}, "wav", 500);
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.body, "{\"text\":\"ola\"}");
    EXPECT_TRUE(r.error.empty());
    EXPECT_EQ(fake->uploads().at(0), "wav");
    EXPECT_EQ(fake->requests().at(0).timeouts.receiveMs, 1000);  // clamped up to 1 s
    auto r2 = http.post("https://api.example.com/v1/audio", {}, "wav", 20000);
    EXPECT_EQ(r2.status, 200);
    EXPECT_EQ(r2.body.size(), static_cast<std::size_t>(4u << 20));
}

TEST(CaptionsNetAdapter, PostTransportError) {
    auto fake = std::make_shared<net::FakeHttpClient>();
    captions::NetBackedHttp http(fake);  // unscripted => transport failure
    auto r = http.post("https://api.example.com/v1/audio", {}, "wav", 5000);
    EXPECT_EQ(r.status, 0);
    EXPECT_FALSE(r.error.empty());
}
