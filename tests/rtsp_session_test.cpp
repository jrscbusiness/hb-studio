#include <gtest/gtest.h>

#include "rtsp/session.hpp"
#include "rtsp_test_support.hpp"

using namespace hb::rtsp;
using namespace hb::rtsp::test;

namespace {
struct SRig {
    FakeRtspServer srv;
    RtspSession s;
    TestClock clk;
    std::vector<std::pair<int, Bytes>> frames;
    bool playingFired = false;

    static SessionConfig sc(const std::string& url, Credentials cred = {}) {
        SessionConfig c;
        c.url = url;
        c.credentials = cred;
        c.cnonce = [] { return std::string("c0ffee00"); };
        return c;
    }
    explicit SRig(FakeServerConfig fc = {}, SessionConfig c = sc("rtsp://cam/stream1")) : srv(std::move(fc)), s(std::move(c)) {
        s.onInterleaved = [this](std::uint8_t ch, ByteView p) { frames.emplace_back(ch, Bytes(p.begin(), p.end())); };
        s.onPlaying = [this] { playingFired = true; };
    }
    void run(int maxRounds = 30) {
        for (int i = 0; i < maxRounds; ++i) {
            Bytes o = s.takeOutput();
            if (o.empty()) break;
            Bytes resp = srv.onBytes(o);
            if (resp.empty()) break;
            s.onBytes(resp, clk.now());
        }
    }
    void begin() {
        s.start(clk.now());
        run();
    }
};
}  // namespace

TEST(Session, FullHandshakeWithoutAuth) {
    SRig r;
    r.begin();
    EXPECT_EQ(r.s.state(), SessionState::Playing);
    EXPECT_TRUE(r.playingFired);
    EXPECT_EQ(r.srv.methods, (std::vector<std::string>{"OPTIONS", "DESCRIBE", "SETUP", "PLAY"}));
    EXPECT_EQ(r.srv.lastSetupUri, "rtsp://cam/stream1/trackID=1");  // Content-Base + a=control
    EXPECT_EQ(r.srv.lastTransport, "RTP/AVP/TCP;unicast;interleaved=0-1");
    EXPECT_EQ(r.srv.requests[3].uri, "rtsp://cam/stream1/");  // PLAY on the aggregate (base) URL
    EXPECT_EQ(r.srv.requests[3].headers.get("Session"), std::optional<std::string>("A1B2C3D4"));
    EXPECT_FALSE(r.srv.requests[1].headers.get("Session").has_value());
    const auto& m = r.s.media();
    EXPECT_EQ(m.codec, Codec::H264);
    EXPECT_EQ(m.payloadType, 96);
    EXPECT_EQ(m.clockRate, 90000);
    EXPECT_EQ(m.paramSets.size(), 2u);
    EXPECT_EQ(m.rtpChannel, 0);
    EXPECT_EQ(m.rtcpChannel, 1);
    EXPECT_EQ(m.ssrc, 0x11223344u);
    EXPECT_EQ(m.initialSeq, std::optional<std::uint16_t>(1000));
    EXPECT_EQ(m.width, 1920);
    EXPECT_DOUBLE_EQ(m.framerate, 25.0);
    EXPECT_EQ(r.s.sessionId(), "A1B2C3D4");
    EXPECT_EQ(r.s.keepaliveInterval(), Millis(30000));  // timeout 60 s -> half
    EXPECT_TRUE(r.s.serverSupportsGetParameter());
    // every request carries CSeq and a User-Agent
    for (auto& rq : r.srv.requests) {
        EXPECT_TRUE(rq.headers.get("CSeq").has_value());
        EXPECT_EQ(rq.headers.get("User-Agent"), std::optional<std::string>("HBStudio/0.1"));
    }
}

TEST(Session, DigestAuthenticationAndCredentialsFromUrl) {
    FakeServerConfig fc;
    fc.auth = FakeServerConfig::Auth::Digest;
    SRig r(fc, SRig::sc("rtsp://admin:s3cretPass@cam/stream1"));
    r.begin();
    EXPECT_EQ(r.s.state(), SessionState::Playing) << r.s.failureMessage();
    EXPECT_EQ(r.srv.unauthorized, 1);  // challenged once, then every request carried a valid header
    EXPECT_EQ(r.srv.count("PLAY"), 1);
    for (auto& rq : r.srv.requests) {  // credentials never travel in the request line
        EXPECT_EQ(rq.uri.find("admin"), std::string::npos);
        EXPECT_EQ(rq.uri.find('@'), std::string::npos);
    }
}

TEST(Session, ExplicitCredentialsOverrideUrl) {
    FakeServerConfig fc;
    fc.auth = FakeServerConfig::Auth::Digest;
    SRig r(fc, SRig::sc("rtsp://wrong:wrong@cam/stream1", {"admin", "s3cretPass"}));
    r.begin();
    EXPECT_EQ(r.s.state(), SessionState::Playing);
}

TEST(Session, BasicAuthentication) {
    FakeServerConfig fc;
    fc.auth = FakeServerConfig::Auth::Basic;
    SRig r(fc, SRig::sc("rtsp://cam/stream1", {"admin", "s3cretPass"}));
    r.begin();
    EXPECT_EQ(r.s.state(), SessionState::Playing);
    auto h = r.srv.requests.back().headers.get("Authorization");
    ASSERT_TRUE(h);
    EXPECT_EQ(h->rfind("Basic ", 0), 0u);
    EXPECT_EQ(h->find("s3cretPass"), std::string::npos);  // base64, not plain text
}

TEST(Session, WrongPasswordFailsWithoutLooping) {
    for (auto scheme : {FakeServerConfig::Auth::Digest, FakeServerConfig::Auth::Basic}) {
        FakeServerConfig fc;
        fc.auth = scheme;
        SRig r(fc, SRig::sc("rtsp://admin:nope@cam/stream1"));
        r.begin();
        EXPECT_EQ(r.s.state(), SessionState::Failed);
        EXPECT_EQ(r.s.failure(), FailureKind::AuthFailed);
        EXPECT_LE(r.srv.requests.size(), 3u);  // original + one authenticated retry
    }
}

TEST(Session, MissingCredentialsIs401Failure) {
    FakeServerConfig fc;
    fc.auth = FakeServerConfig::Auth::Digest;
    SRig r(fc);
    r.begin();
    EXPECT_EQ(r.s.failure(), FailureKind::AuthFailed);
    EXPECT_NE(r.s.failureMessage().find("credentials"), std::string::npos);
    EXPECT_EQ(r.srv.requests.size(), 1u);
}

TEST(Session, StaleNonceIsRetriedTransparently) {
    FakeServerConfig fc;
    fc.auth = FakeServerConfig::Auth::Digest;
    fc.staleOnce = true;
    SRig r(fc, SRig::sc("rtsp://admin:s3cretPass@cam/stream1"));
    r.begin();
    EXPECT_EQ(r.s.state(), SessionState::Playing) << r.s.failureMessage();
    EXPECT_EQ(r.srv.unauthorized, 2);
}

TEST(Session, NotFound404) {
    SRig r({}, SRig::sc("rtsp://cam/doesnotexist"));
    r.begin();
    EXPECT_EQ(r.s.state(), SessionState::Failed);
    EXPECT_EQ(r.s.failure(), FailureKind::NotFound);
}

TEST(Session, RedirectIsReportedWithTarget) {
    FakeServerConfig fc;
    fc.redirectTo = "rtsp://other-cam:8554/stream1";
    SRig r(fc);
    r.begin();
    EXPECT_EQ(r.s.failure(), FailureKind::Redirect);
    EXPECT_EQ(r.s.redirectUrl(), "rtsp://other-cam:8554/stream1");
}

TEST(Session, RedirectWithoutValidLocationIsBadResponse) {
    FakeServerConfig fc;
    fc.redirectTo = "http://nonsense";
    SRig r(fc);
    r.begin();
    EXPECT_EQ(r.s.failure(), FailureKind::BadResponse);
}

TEST(Session, SetupRefusals) {
    {
        FakeServerConfig fc;
        fc.setupStatus = 461;
        SRig r(fc);
        r.begin();
        EXPECT_EQ(r.s.failure(), FailureKind::UnsupportedTransport);
    }
    {
        FakeServerConfig fc;
        fc.replyTcpAsUdp = true;  // server ignores our TCP request
        SRig r(fc);
        r.begin();
        EXPECT_EQ(r.s.failure(), FailureKind::UnsupportedTransport);
    }
    {
        FakeServerConfig fc;
        fc.setupStatus = 454;
        SRig r(fc);
        r.begin();
        EXPECT_EQ(r.s.failure(), FailureKind::SessionLost);
    }
    {
        FakeServerConfig fc;
        fc.setupStatus = 503;
        SRig r(fc);
        r.begin();
        EXPECT_EQ(r.s.failure(), FailureKind::ServerError);
    }
}

TEST(Session, UdpModeSendsClientPortsAndParsesServerPorts) {
    SessionConfig c = SRig::sc("rtsp://cam/stream1");
    c.transport = TransportMode::Udp;
    c.udpClientPort = 51000;
    FakeServerConfig fc;
    fc.replyTcpAsUdp = true;
    SRig r(fc, c);
    r.begin();
    EXPECT_EQ(r.srv.lastTransport, "RTP/AVP;unicast;client_port=51000-51001");
    EXPECT_EQ(r.s.state(), SessionState::Playing) << r.s.failureMessage();
    EXPECT_EQ(r.s.media().serverRtpPort, 6970);
    EXPECT_EQ(r.s.media().serverRtcpPort, 6971);
}

TEST(Session, NoSupportedMedia) {
    FakeServerConfig fc;
    fc.sdp = "v=0\r\nm=audio 0 RTP/AVP 0\r\na=rtpmap:0 PCMU/8000\r\n";
    SRig r(fc);
    r.begin();
    EXPECT_EQ(r.s.failure(), FailureKind::NoSupportedMedia);
    FakeServerConfig fc2;
    fc2.sdp = "this is not sdp";
    SRig r2(fc2);
    r2.begin();
    EXPECT_EQ(r2.s.failure(), FailureKind::BadResponse);
    FakeServerConfig fc3;
    fc3.sdp = "v=0\r\nm=video 0 RTP/SAVP 96\r\na=rtpmap:96 H264/90000\r\n";
    SRig r3(fc3);
    r3.begin();
    EXPECT_EQ(r3.s.failure(), FailureKind::NoSupportedMedia);
    FakeServerConfig fc4;
    fc4.sdp = "v=0\r\nm=video 0 RTP/AVP 96\r\na=rtpmap:96 H265/90000\r\na=fmtp:96 sprop-max-don-diff=2\r\n";
    SRig r4(fc4);
    r4.begin();
    EXPECT_EQ(r4.s.failure(), FailureKind::NoSupportedMedia);
}

TEST(Session, OptionsNotImplementedIsTolerated) {
    FakeServerConfig fc;
    fc.optionsStatus = 405;
    SRig r(fc);
    r.begin();
    EXPECT_EQ(r.s.state(), SessionState::Playing);
    EXPECT_FALSE(r.s.serverSupportsGetParameter());
}

TEST(Session, RequestTimeoutFailsTheHandshake) {
    FakeServerConfig fc;
    fc.mute.insert("DESCRIBE");
    SRig r(fc);
    r.begin();
    EXPECT_EQ(r.s.state(), SessionState::Describe);
    r.clk.advance(Millis(4999));
    r.s.poll(r.clk.now());
    EXPECT_EQ(r.s.state(), SessionState::Describe);
    r.clk.advance(Millis(2));
    r.s.poll(r.clk.now());
    EXPECT_EQ(r.s.failure(), FailureKind::Timeout);
}

TEST(Session, KeepaliveUsesGetParameterThenOptions) {
    SRig r;
    r.begin();
    r.clk.advance(Millis(29999));
    r.s.poll(r.clk.now());
    EXPECT_EQ(r.s.keepalivesSent(), 0);
    r.clk.advance(Millis(2));
    r.s.poll(r.clk.now());
    EXPECT_EQ(r.s.keepalivesSent(), 1);
    r.run();
    EXPECT_EQ(r.srv.methods.back(), "GET_PARAMETER");
    EXPECT_EQ(r.srv.requests.back().headers.get("Session"), std::optional<std::string>("A1B2C3D4"));
    r.clk.advance(Millis(30000));
    r.s.poll(r.clk.now());
    r.run();
    EXPECT_EQ(r.srv.count("GET_PARAMETER"), 2);
    EXPECT_EQ(r.s.state(), SessionState::Playing);

    FakeServerConfig fc;
    fc.supportGetParameter = false;
    fc.sessionTimeoutSec = 20;
    SRig o(fc);
    o.begin();
    EXPECT_EQ(o.s.keepaliveInterval(), Millis(10000));
    o.clk.advance(Millis(10001));
    o.s.poll(o.clk.now());
    o.run();
    EXPECT_EQ(o.srv.methods.back(), "OPTIONS");
    EXPECT_EQ(o.srv.count("OPTIONS"), 2);
}

TEST(Session, KeepaliveIntervalIsClamped) {
    FakeServerConfig fc;
    fc.sessionTimeoutSec = 4;
    SRig a(fc);
    a.begin();
    EXPECT_EQ(a.s.keepaliveInterval(), Millis(5000));
    fc.sessionTimeoutSec = 3600;
    SRig b(fc);
    b.begin();
    EXPECT_EQ(b.s.keepaliveInterval(), Millis(30000));
}

TEST(Session, Keepalive454MeansSessionLost) {
    SRig r;
    r.begin();
    r.srv.config().sessionExpired = true;
    r.clk.advance(Millis(30001));
    r.s.poll(r.clk.now());
    r.run();
    EXPECT_EQ(r.s.state(), SessionState::Failed);
    EXPECT_EQ(r.s.failure(), FailureKind::SessionLost);
}

TEST(Session, UnansweredKeepaliveIsNotFatalAndIsRetried) {
    SRig r;
    r.begin();
    r.srv.config().mute.insert("GET_PARAMETER");
    r.clk.advance(Millis(30001));
    r.s.poll(r.clk.now());
    r.run();
    EXPECT_EQ(r.s.keepalivesSent(), 1);
    r.clk.advance(Millis(5001));
    r.s.poll(r.clk.now());
    EXPECT_EQ(r.s.state(), SessionState::Playing);  // the data watchdog (RtspInput) decides, not the session
}

TEST(Session, KeepaliveCanReauthenticate) {
    FakeServerConfig fc;
    fc.auth = FakeServerConfig::Auth::Digest;
    SRig r(fc, SRig::sc("rtsp://admin:s3cretPass@cam/stream1"));
    r.begin();
    ASSERT_EQ(r.s.state(), SessionState::Playing);
    r.srv.config().staleOnce = true;  // server rotates the nonce
    r.clk.advance(Millis(30001));
    r.s.poll(r.clk.now());
    r.run();
    EXPECT_EQ(r.s.state(), SessionState::Playing);
    EXPECT_GE(r.srv.count("GET_PARAMETER"), 2);  // original + authenticated retry
}

TEST(Session, InterleavedFramesOnlyAfterSetupAndFragmentationSafe) {
    SRig r;
    r.s.start(r.clk.now());
    // Deliver every server answer one byte at a time.
    for (int i = 0; i < 10 && r.s.state() != SessionState::Playing; ++i) {
        Bytes o = r.s.takeOutput();
        Bytes resp = r.srv.onBytes(o);
        for (auto b : resp) r.s.onBytes(ByteView(&b, 1), r.clk.now());
    }
    ASSERT_EQ(r.s.state(), SessionState::Playing);
    Bytes rtp = buildRtp(96, true, 1000, 0, 1, Bytes{1, 2, 3});
    Bytes fr = frameInterleaved(0, rtp);
    for (auto b : fr) r.s.onBytes(ByteView(&b, 1), r.clk.now());
    ASSERT_EQ(r.frames.size(), 1u);
    EXPECT_EQ(r.frames[0].first, 0);
    EXPECT_EQ(r.frames[0].second, rtp);
}

TEST(Session, AnswersServerInitiatedRequests) {
    SRig r;
    r.begin();
    std::string ping = "GET_PARAMETER rtsp://cam/stream1/ RTSP/1.0\r\nCSeq: 55\r\nSession: A1B2C3D4\r\n\r\n";
    r.s.onBytes(toBytes(ping), r.clk.now());
    Bytes out = r.s.takeOutput();
    std::string s(out.begin(), out.end());
    EXPECT_NE(s.find("RTSP/1.0 200 OK"), std::string::npos);
    EXPECT_NE(s.find("CSeq: 55"), std::string::npos);
    r.s.onBytes(toBytes("ANNOUNCE rtsp://cam/x RTSP/1.0\r\nCSeq: 56\r\n\r\n"), r.clk.now());
    out = r.s.takeOutput();
    EXPECT_NE(std::string(out.begin(), out.end()).find("501"), std::string::npos);
    EXPECT_EQ(r.s.state(), SessionState::Playing);
}

TEST(Session, TeardownOnStop) {
    SRig r;
    r.begin();
    r.s.stop(r.clk.now());
    EXPECT_EQ(r.s.state(), SessionState::Teardown);
    r.run();
    EXPECT_EQ(r.srv.methods.back(), "TEARDOWN");
    EXPECT_EQ(r.s.state(), SessionState::Closed);
    EXPECT_FALSE(r.srv.playing);
}

TEST(Session, TeardownTimesOutToClosed) {
    FakeServerConfig fc;
    fc.mute.insert("TEARDOWN");
    SRig r(fc);
    r.begin();
    r.s.stop(r.clk.now());
    r.run();
    r.clk.advance(Millis(1001));
    r.s.poll(r.clk.now());
    EXPECT_EQ(r.s.state(), SessionState::Closed);
}

TEST(Session, GarbageAndClosureAndBadUrl) {
    SRig r;
    r.s.start(r.clk.now());
    r.s.onBytes(toBytes("\x01\x02 definitely not rtsp"), r.clk.now());
    EXPECT_EQ(r.s.failure(), FailureKind::ProtocolError);

    SRig c;
    c.begin();
    c.s.onConnectionClosed();
    EXPECT_EQ(c.s.failure(), FailureKind::ConnectionClosed);

    SRig bad({}, SRig::sc("http://cam/x"));
    bad.s.start(bad.clk.now());
    EXPECT_EQ(bad.s.failure(), FailureKind::Config);
}

TEST(Session, ResponsesWithUnknownCseqAreIgnored) {
    SRig r;
    r.s.start(r.clk.now());
    r.s.onBytes(toBytes("RTSP/1.0 404 Not Found\r\nCSeq: 999\r\n\r\n"), r.clk.now());
    EXPECT_EQ(r.s.state(), SessionState::Options);  // stale answer did not derail the handshake
    r.run();
    EXPECT_EQ(r.s.state(), SessionState::Playing);
}

TEST(Session, ContentBaseMissingFallsBackToRequestUri) {
    FakeServerConfig fc;
    fc.contentBase = "rtsp://cam/stream1/";
    SRig r(fc);
    r.begin();
    EXPECT_EQ(r.srv.lastSetupUri, "rtsp://cam/stream1/trackID=1");
}
