// RTMPS: rtmps:// URL parsing, and the production RtmpClient publishing through a TlsStream backed by
// the fake record-protocol engine, against the scripted RTMP server over a real loopback socket (the
// server side is wrapped in a server-role TlsStream, so both directions are "encrypted").
#include <gtest/gtest.h>

#include <atomic>
#include <future>
#include <thread>

#include "fake_tls.hpp"
#include "rtmp_test_support.hpp"
#include "streaming/flv.hpp"
#include "streaming/rtmp_client.hpp"

using namespace hb::streaming;
using namespace hb::streaming::test;

TEST(Rtmps, UrlParsing) {
    auto a = parseRtmpUrl("rtmps://a.rtmps.youtube.com/live2");
    EXPECT_TRUE(a.ok);
    EXPECT_TRUE(a.tls);
    EXPECT_EQ(a.port, 443);
    EXPECT_FALSE(a.portExplicit);
    EXPECT_EQ(a.host, "a.rtmps.youtube.com");
    EXPECT_EQ(a.app, "live2");

    auto b = parseRtmpUrl("rtmps://live.twitch.tv:443/app");
    EXPECT_TRUE(b.ok); EXPECT_TRUE(b.tls); EXPECT_EQ(b.port, 443); EXPECT_TRUE(b.portExplicit);

    auto c = parseRtmpUrl("rtmps://example.com:8443/app/sub");
    EXPECT_TRUE(c.ok); EXPECT_EQ(c.port, 8443); EXPECT_EQ(c.app, "app/sub");

    auto d = parseRtmpUrl("RTMPS://Example.com/app");   // scheme is case-insensitive
    EXPECT_TRUE(d.ok); EXPECT_TRUE(d.tls);

    auto e = parseRtmpUrl("rtmp://example.com/app");
    EXPECT_TRUE(e.ok); EXPECT_FALSE(e.tls); EXPECT_EQ(e.port, 1935);

    EXPECT_FALSE(parseRtmpUrl("srt://example.com:9000/app").ok);     // not an RTMP scheme
    EXPECT_FALSE(parseRtmpUrl("http://example.com/app").ok);
    EXPECT_FALSE(parseRtmpUrl("rtmps://example.com").ok);            // no app
    EXPECT_FALSE(parseRtmpUrl("rtmps://:443/app").ok);               // no host
}

TEST(Rtmps, TransportFactory) {
    std::string why;
    EXPECT_NE(makeStreamTransport("rtmp", &why), nullptr);
    EXPECT_NE(makeStreamTransport("rtmps", &why), nullptr);
    EXPECT_EQ(makeStreamTransport("srt", &why), nullptr);
    EXPECT_NE(why.find("SRT"), std::string::npos);
    EXPECT_EQ(makeStreamTransport("carrier-pigeon", &why), nullptr);
}

namespace {

// Serves one RTMP client behind a server-role fake TLS layer. certName is what the "certificate" says.
struct TlsServer {
    std::unique_ptr<hb::rtsp::TcpListener> listener = hb::rtsp::makeLoopbackListener();
    std::atomic<bool> stop{false};
    std::future<FakeRtmpServerResult> result;

    void start(const std::string& certName) {
        result = std::async(std::launch::async, [this, certName] {
            auto conn = listener->accept(3000);
            if (!conn) return FakeRtmpServerResult{};
            TlsStream tls(std::make_unique<ConnectedPassthrough>(std::move(conn)),
                          std::make_unique<FakeTlsEngine>(FakeTlsEngine::Role::Server, certName));
            tls.connect("", 0);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(3000);
            hb::rtsp::IoStatus st = hb::rtsp::IoStatus::WouldBlock;
            while (st == hb::rtsp::IoStatus::WouldBlock && std::chrono::steady_clock::now() < deadline && !stop) {
                tls.waitReadable(20);
                st = tls.pollConnect();
            }
            if (st != hb::rtsp::IoStatus::Ok) return FakeRtmpServerResult{};
            return serveOneRtmpClient(tls, stop, 4000);
        });
    }
};

RtmpClient::TlsEngineFactory fakeClientEngine() {
    return [] { return std::make_unique<FakeTlsEngine>(FakeTlsEngine::Role::Client); };
}

}  // namespace

TEST(Rtmps, PublishesVideoAndAudioThroughTls) {
    TlsServer srv;
    ASSERT_TRUE(srv.listener);
    srv.start("127.0.0.1");

    RtmpClient client;
    client.setTlsEngineFactory(fakeClientEngine());
    RtmpTarget t;
    t.host = "127.0.0.1";
    t.port = srv.listener->port();
    t.app = "live2";
    t.streamKey = "secret-key-9";
    t.tls = true;
    ASSERT_TRUE(client.start(t, 3000)) << client.lastError();
    EXPECT_TRUE(client.isPublishing());

    flv::NalUnit sps{0x67, 0x42, 0, 0x1E}, pps{0x68, 0xCE};
    ASSERT_TRUE(client.sendVideo(0, flv::buildAvcSequenceHeader(sps, pps)));
    for (std::uint32_t ts = 0; ts < 3; ++ts) {
        flv::NalUnit idr(3000, static_cast<std::uint8_t>(ts));   // spans chunks AND many fake TLS records
        ASSERT_TRUE(client.sendVideo(ts * 33, flv::buildAvcVideoTag({idr}, ts == 0)));
    }
    ASSERT_TRUE(client.sendAudio(0, flv::buildAacSequenceHeader({0x12, 0x10})));
    ASSERT_TRUE(client.sendAudio(20, flv::buildAacAudioTag(flv::Bytes(120, 0x11))));

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    srv.stop = true;
    const auto r = srv.result.get();
    EXPECT_TRUE(r.connected);
    EXPECT_TRUE(r.published);
    EXPECT_EQ(r.lastStreamKey, "secret-key-9");
    EXPECT_EQ(r.videoMessages, 4);
    EXPECT_EQ(r.audioMessages, 2);
}

TEST(Rtmps, OpenWithRtmpsUrlUsesTlsAndExplicitPort) {
    TlsServer srv;
    ASSERT_TRUE(srv.listener);
    srv.start("127.0.0.1");
    RtmpClient client;
    client.setTlsEngineFactory(fakeClientEngine());
    const std::string url = "rtmps://127.0.0.1:" + std::to_string(srv.listener->port()) + "/live2";
    ASSERT_TRUE(client.open(url, "k", 3000)) << client.lastError();
    EXPECT_TRUE(client.isPublishing());
    srv.stop = true;
    EXPECT_TRUE(srv.result.get().published);
}

TEST(Rtmps, CertificateNameMismatchIsRefusedAndPermanent) {
    TlsServer srv;
    ASSERT_TRUE(srv.listener);
    srv.start("not-the-host.example");   // the "certificate" is for a different name
    RtmpClient client;
    client.setTlsEngineFactory(fakeClientEngine());
    RtmpTarget t;
    t.host = "127.0.0.1";
    t.port = srv.listener->port();
    t.app = "live2";
    t.tls = true;
    EXPECT_FALSE(client.start(t, 3000));
    EXPECT_FALSE(client.isPublishing());
    EXPECT_TRUE(client.permanentFailure());   // never hammer a server whose identity we reject
    EXPECT_NE(client.lastError().find("certificado"), std::string::npos) << client.lastError();
    srv.stop = true;
    (void)srv.result.get();
}

TEST(Rtmps, NoTlsBackendIsAnErrorNotAPlaintextFallback) {
    RtmpClient client;
    client.setTlsEngineFactory([]() -> std::unique_ptr<ITlsEngine> { return nullptr; });
    RtmpTarget t;
    t.host = "127.0.0.1";
    t.port = 1;   // would fail anyway; the point is that we never even try plaintext
    t.app = "a";
    t.tls = true;
    EXPECT_FALSE(client.start(t, 500));
    EXPECT_TRUE(client.permanentFailure());
    EXPECT_NE(client.lastError().find("RTMPS"), std::string::npos);
}

TEST(Rtmps, InvalidUrlIsPermanentFailure) {
    RtmpClient client;
    EXPECT_FALSE(client.open("srt://host:1/app", "k", 500));
    EXPECT_TRUE(client.permanentFailure());
}
