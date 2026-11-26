#include <gtest/gtest.h>

#include <filesystem>

#include "cloud/net_adapter.hpp"
#include "net/http_fake.hpp"

using namespace hb;

namespace {
struct Gate : cloud::ITransferGate {
    std::size_t total = 0;
    int allow = 1000;
    bool acquire(std::size_t n) override {
        if (allow-- <= 0) return false;
        total += n;
        return true;
    }
};
}  // namespace

TEST(CloudNetAdapter, RequestPolicyAndHeaderFiltering) {
    auto fake = std::make_shared<net::FakeHttpClient>();
    net::FakeHttpClient::Reply rep;
    rep.status = 308;
    rep.headers = {{"Range", "bytes=0-9"}};
    fake->enqueue(rep);
    auto http = cloud::makeCloudHttpOverNet(fake);
    cloud::HttpRequest r;
    r.method = "PUT";
    r.url = "https://upload.example.com/x";
    r.headers = {{"Host", "evil"}, {"content-length", "1"}, {"Authorization", "Bearer t"}};
    r.body = "data";
    const auto res = http->send(r);
    ASSERT_TRUE(res.ok());
    EXPECT_EQ(res.value().status, 308);  // 3xx handed back: redirects are never followed
    EXPECT_EQ(cloud::headerValue(res.value(), "range"), "bytes=0-9");
    const auto sent = fake->requests().at(0);
    EXPECT_EQ(sent.redirects, net::RedirectPolicy::Never);
    ASSERT_EQ(sent.headers.size(), 1u);
    EXPECT_EQ(sent.headers[0].first, "Authorization");
    EXPECT_EQ(sent.timeouts.connectMs, 30000);
    EXPECT_EQ(fake->uploads().at(0), "data");
}

TEST(CloudNetAdapter, GateIsConsultedPerSliceAndCancels) {
    auto fake = std::make_shared<net::FakeHttpClient>();
    fake->enqueue({});
    fake->enqueue({});
    auto http = cloud::makeCloudHttpOverNet(fake);
    cloud::HttpRequest r;
    r.method = "PUT";
    r.url = "https://upload.example.com/x";
    r.body = std::string(100 * 1024, 'q');
    Gate gate;
    r.gate = &gate;
    ASSERT_TRUE(http->send(r).ok());
    EXPECT_EQ(gate.total, r.body.size());
    EXPECT_EQ(fake->uploads().at(0), r.body);
    Gate stop;
    stop.allow = 1;  // second slice refused
    r.gate = &stop;
    const auto c = http->send(r);
    ASSERT_FALSE(c.ok());
    EXPECT_EQ(c.error().kind, cloud::ErrorKind::Cancelled);
}

TEST(CloudNetAdapter, ErrorMapping) {
    auto fake = std::make_shared<net::FakeHttpClient>();
    net::FakeHttpClient::Reply to;
    to.error = net::ErrorKind::Timeout;
    fake->enqueue(to);
    auto http = cloud::makeCloudHttpOverNet(fake);
    cloud::HttpRequest r;
    r.url = "https://x.example.com/";
    auto a = http->send(r);
    ASSERT_FALSE(a.ok());
    EXPECT_EQ(a.error().kind, cloud::ErrorKind::Transient);
    r.url = "gopher://x";
    auto b = http->send(r);
    ASSERT_FALSE(b.ok());
    EXPECT_EQ(b.error().kind, cloud::ErrorKind::Permanent);
}

TEST(CloudNetAdapter, FileSecretStoreRoundTripsThroughProtector) {
    const std::string path = (std::filesystem::path(::testing::TempDir()) / "cloud_secrets_net.json").string();
    std::filesystem::remove(path);
    auto prot = std::make_shared<net::FakeSecretStore>();
    {
        auto s = cloud::makeFileSecretStore(path, prot);
        ASSERT_TRUE(s->put("s3.secret", "TOPSECRET"));
        EXPECT_EQ(s->get("s3.secret").value_or(""), "TOPSECRET");
        EXPECT_FALSE(s->get("missing").has_value());
    }
    {
        auto s = cloud::makeFileSecretStore(path, prot);  // persisted
        EXPECT_EQ(s->get("s3.secret").value_or(""), "TOPSECRET");
        EXPECT_TRUE(s->erase("s3.secret"));
        EXPECT_FALSE(s->erase("s3.secret"));
    }
    prot->failProtect = true;
    EXPECT_FALSE(cloud::makeFileSecretStore(path, prot)->put("k", "v"));
    std::filesystem::remove(path);
}
