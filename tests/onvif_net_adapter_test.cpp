#include <gtest/gtest.h>

#include "net/http_fake.hpp"
#include "onvif/net_adapter.hpp"

using namespace hb;

TEST(OnvifNetAdapter, PassesStatusAndBodyIncludingHttpErrors) {
    auto fake = std::make_shared<net::FakeHttpClient>();
    net::FakeHttpClient::Reply rep;
    rep.status = 400;
    rep.body = "<fault/>";
    fake->enqueue(rep);
    auto http = onvif::makeOnvifHttpOverNet(fake);
    onvif::HttpRequest r;
    r.url = "http://192.168.1.10/onvif/device_service";
    r.body = "<soap/>";
    const auto res = http->send(r);
    ASSERT_TRUE(res.ok());
    EXPECT_EQ(res.value().status, 400);
    EXPECT_EQ(res.value().body, "<fault/>");
    EXPECT_EQ(fake->uploads().at(0), "<soap/>");
}

TEST(OnvifNetAdapter, MapsErrors) {
    auto fake = std::make_shared<net::FakeHttpClient>();
    net::FakeHttpClient::Reply to;
    to.error = net::ErrorKind::Timeout;
    fake->enqueue(to);
    net::FakeHttpClient::Reply big;
    big.body = std::string(onvif::kMaxHttpResponseBytes + 1, 'x');
    fake->enqueue(big);
    auto http = onvif::makeOnvifHttpOverNet(fake);
    onvif::HttpRequest r;
    r.url = "http://cam/x";
    auto a = http->send(r);
    ASSERT_FALSE(a.ok());
    EXPECT_EQ(a.error().code, onvif::ErrorCode::Timeout);
    auto b = http->send(r);
    ASSERT_FALSE(b.ok());
    EXPECT_EQ(b.error().code, onvif::ErrorCode::Transport);
    r.url = "ftp://cam/x";
    auto c = http->send(r);
    ASSERT_FALSE(c.ok());
    EXPECT_EQ(c.error().code, onvif::ErrorCode::InvalidArgument);
}
