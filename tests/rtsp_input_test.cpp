#include <gtest/gtest.h>

#include <sstream>

#include "rtsp/rtsp_input.hpp"
#include "rtsp_test_support.hpp"

using namespace hb::rtsp;
using namespace hb::rtsp::test;
using hb::core::LatencyMode;

namespace {
struct IRig {
    FakeNetwork net;
    TestClock clk;
    RtspInputConfig cfg;
    std::unique_ptr<RtspInput> in;
    std::vector<std::pair<InputState, InputState>> transitions;
    std::vector<TimePoint> attempts;  // time of every TCP connection attempt
    double rnd = 0.5;                 // 0.5 => zero backoff jitter
    std::shared_ptr<hb::core::MemorySink> sink = std::make_shared<hb::core::MemorySink>();
    Packetizer pk;
    std::uint32_t ts = 0;

    explicit IRig(std::function<void(RtspInputConfig&, FakeNetwork&)> tweak = nullptr) {
        cfg.url = "rtsp://admin:s3cretPass@192.168.1.64:554/stream1";
        if (tweak) tweak(cfg, net);
        auto logger = std::make_shared<hb::core::Logger>();
        logger->setMinLevel(hb::core::Level::Debug);
        logger->addSink(sink);
        InputDeps d;
        auto base = net.factory();
        d.streamFactory = [this, base] {
            attempts.push_back(clk.now());
            return base();
        };
        d.logger = logger;
        d.random01 = [this] { return rnd; };
        d.cnonce = [] { return std::string("c0ffee00"); };
        in = std::make_unique<RtspInput>(cfg, d);
        in->setStateCallback([this](InputState f, InputState t, const std::string&) { transitions.emplace_back(f, t); });
    }

    void run(Millis total, Millis step = Millis(10)) {
        for (Millis e{0}; e < total; e += step) {
            in->tick(clk.now());
            clk.advance(step);
        }
    }
    void startAndHandshake() {
        in->start(clk.now());
        run(Millis(100));
    }
    std::shared_ptr<FakeConnection> conn() { return net.last(); }
    void frame(bool key, std::size_t size = 60, Millis advance = Millis(0)) {
        ts += 3600;
        auto pkts = pk.h264({key ? idr264(size) : p264(size)}, ts);
        for (auto& p : pkts) conn()->serverSendInterleaved(0, p);
        if (advance.count()) run(advance);
    }
    InputState state() { return in->health(clk.now()).state; }
    std::string logs() {
        std::ostringstream o;
        sink->exportTo(o);
        return o.str();
    }
};

std::vector<long long> gapsMs(const std::vector<TimePoint>& t) {
    std::vector<long long> g;
    for (std::size_t i = 1; i < t.size(); ++i)
        g.push_back(std::chrono::duration_cast<Millis>(t[i] - t[i - 1]).count());
    return g;
}
}  // namespace

TEST(Input, ConnectsThenGoesOnlineOnFirstKeyframe) {
    IRig r;
    EXPECT_EQ(r.in->state(), InputState::Stopped);
    r.startAndHandshake();
    EXPECT_EQ(r.state(), InputState::Connecting);  // PLAY acknowledged, but no video yet
    ASSERT_EQ(r.net.connections.size(), 1u);
    EXPECT_EQ(r.conn()->target, "192.168.1.64:554");
    EXPECT_TRUE(r.conn()->server.playing);
    r.frame(true, 60, Millis(30));
    EXPECT_EQ(r.state(), InputState::Online);
    auto au = r.in->popAccessUnit();
    ASSERT_TRUE(au);
    EXPECT_TRUE(au->keyframe);
    EXPECT_EQ(au->data, annexB({sps264(), pps264(), idr264(60)}));
    EXPECT_FALSE(r.in->popAccessUnit());
    r.frame(false, 30, Millis(30));
    EXPECT_EQ(r.in->popAccessUnit()->data, annexB({p264(30)}));
    ASSERT_EQ(r.transitions.size(), 2u);
    EXPECT_EQ(r.transitions[0], std::make_pair(InputState::Stopped, InputState::Connecting));
    EXPECT_EQ(r.transitions[1], std::make_pair(InputState::Connecting, InputState::Online));
    auto h = r.in->health(r.clk.now());
    EXPECT_EQ(h.level, HealthLevel::Good);
    EXPECT_EQ(h.accessUnits, 2u);
    EXPECT_EQ(h.keyframes, 1u);
    EXPECT_GT(h.bitrateKbps, 0.0);
    EXPECT_EQ(h.maskedUrl, "rtsp://***@192.168.1.64:554/stream1");
}

TEST(Input, DigestAuthenticatedStreamEndToEnd) {
    IRig r([](RtspInputConfig&, FakeNetwork& n) { n.serverCfg.auth = FakeServerConfig::Auth::Digest; });
    r.startAndHandshake();
    r.frame(true, 60, Millis(30));
    EXPECT_EQ(r.state(), InputState::Online);
    EXPECT_EQ(r.conn()->server.unauthorized, 1);
}

TEST(Input, WorksWithTinyReadChunks) {
    IRig r([](RtspInputConfig&, FakeNetwork& n) {
        n.recvChunk = 7;
        n.serverCfg.auth = FakeServerConfig::Auth::Basic;
    });
    r.in->start(r.clk.now());
    r.run(Millis(300));
    r.frame(true, 700);
    r.run(Millis(300));
    EXPECT_EQ(r.state(), InputState::Online);
    EXPECT_EQ(r.in->popAccessUnit()->data, annexB({sps264(), pps264(), idr264(700)}));
}

TEST(Input, PacketLossDropsDamagedFramesAndRecoversOnNextKeyframe) {
    IRig r;
    r.pk.mtu = 100;
    r.startAndHandshake();
    r.frame(true, 300, Millis(30));
    ASSERT_TRUE(r.in->popAccessUnit());
    // lose one fragment of the next frame
    r.ts += 3600;
    auto pkts = r.pk.h264({p264(400)}, r.ts);
    pkts.erase(pkts.begin() + 2);
    for (auto& p : pkts) r.conn()->serverSendInterleaved(0, p);
    r.run(Millis(60));  // > TCP gap wait
    r.frame(false, 50, Millis(30));
    r.frame(false, 50, Millis(30));
    EXPECT_FALSE(r.in->popAccessUnit());  // nothing decodable until the next IDR
    r.frame(true, 300, Millis(30));
    auto au = r.in->popAccessUnit();
    ASSERT_TRUE(au);
    EXPECT_TRUE(au->keyframe);
    auto h = r.in->health(r.clk.now());
    EXPECT_GE(h.rtp.lost, 1u);
    EXPECT_GT(h.lossPercent, 0.0);
    EXPECT_EQ(r.state(), InputState::Online);  // loss alone never drops the connection
}

TEST(Input, ConnectionDropTriggersReconnectAfterOneSecond) {
    IRig r;
    r.startAndHandshake();
    r.frame(true, 60, Millis(30));
    ASSERT_EQ(r.state(), InputState::Online);
    TimePoint dropAt = r.clk.now();
    r.conn()->closeFromServer();
    r.run(Millis(20));
    EXPECT_EQ(r.state(), InputState::Reconnecting);
    EXPECT_EQ(r.in->health(r.clk.now()).lastFailure, FailureKind::ConnectionClosed);
    r.run(Millis(1100));
    ASSERT_EQ(r.net.connections.size(), 2u);
    auto delay = std::chrono::duration_cast<Millis>(r.attempts[1] - dropAt).count();
    EXPECT_GE(delay, 1000);
    EXPECT_LE(delay, 1030);
    r.run(Millis(100));
    r.frame(true, 60, Millis(30));
    EXPECT_EQ(r.state(), InputState::Online);
    EXPECT_EQ(r.in->health(r.clk.now()).reconnects, 1u);
    // ERROR is visible between ONLINE and RECONNECTING
    std::vector<InputState> seq;
    for (auto& t : r.transitions) seq.push_back(t.second);
    EXPECT_EQ(seq, (std::vector<InputState>{InputState::Connecting, InputState::Online, InputState::Error,
                                            InputState::Reconnecting, InputState::Online}));
}

TEST(Input, BackoffDoublesUpToFifteenSecondsCeiling) {
    IRig r([](RtspInputConfig&, FakeNetwork& n) { n.refuseNext = 100; });
    r.in->start(r.clk.now());
    r.run(Millis(110000));
    ASSERT_GE(r.attempts.size(), 9u);
    auto g = gapsMs(r.attempts);
    EXPECT_NEAR(double(g[0]), 1000, 20);
    EXPECT_NEAR(double(g[1]), 2000, 20);
    EXPECT_NEAR(double(g[2]), 4000, 20);
    EXPECT_NEAR(double(g[3]), 8000, 20);
    for (std::size_t i = 4; i < g.size(); ++i) EXPECT_NEAR(double(g[i]), 15000, 20);  // ceiling
    EXPECT_EQ(r.state(), InputState::Reconnecting);
    EXPECT_NE(r.in->health(r.clk.now()).lastError.find("refused"), std::string::npos);
}

TEST(Input, BackoffJitterIsBoundedAndNeverExceedsCeiling) {
    for (double rnd : {0.0, 1.0}) {
        IRig r([](RtspInputConfig&, FakeNetwork& n) { n.refuseNext = 100; });
        r.rnd = rnd;
        r.in->start(r.clk.now());
        r.run(Millis(90000));
        auto g = gapsMs(r.attempts);
        ASSERT_GE(g.size(), 6u);
        double f = rnd == 0.0 ? 0.8 : 1.2;
        EXPECT_NEAR(double(g[0]), 1000 * f, 20);
        EXPECT_NEAR(double(g[1]), 2000 * f, 20);
        EXPECT_NEAR(double(g[2]), 4000 * f, 20);
        for (auto x : g) EXPECT_LE(x, 15020);
        EXPECT_NEAR(double(g.back()), rnd == 0.0 ? 12000 : 15000, 20);
    }
}

TEST(Input, BackoffResetsOnlyAfterStableOperation) {
    IRig r([](RtspInputConfig&, FakeNetwork& n) { n.refuseNext = 2; });  // waits: 1 s, 2 s
    r.in->start(r.clk.now());
    r.run(Millis(3300));
    ASSERT_EQ(r.net.connections.size(), 3u);
    r.run(Millis(100));
    r.frame(true, 60, Millis(30));
    ASSERT_EQ(r.state(), InputState::Online);
    // Flaps right away: still on the escalated schedule (next delay 4 s).
    TimePoint t = r.clk.now();
    r.conn()->closeFromServer();
    r.run(Millis(4200));
    ASSERT_EQ(r.attempts.size(), 4u);
    EXPECT_NEAR(double(std::chrono::duration_cast<Millis>(r.attempts[3] - t).count()), 4000, 30);

    // Now stay healthy for longer than stableAfter (10 s) and drop again: back to 1 s.
    r.run(Millis(100));
    r.frame(true, 60, Millis(30));
    ASSERT_EQ(r.state(), InputState::Online);
    for (int i = 0; i < 120; ++i) r.frame(i % 30 == 0, 40, Millis(100));  // 12 s of video
    ASSERT_EQ(r.state(), InputState::Online);
    t = r.clk.now();
    r.conn()->closeFromServer();
    r.run(Millis(1200));
    ASSERT_EQ(r.attempts.size(), 5u);
    EXPECT_NEAR(double(std::chrono::duration_cast<Millis>(r.attempts[4] - t).count()), 1000, 30);
}

TEST(Input, WatchdogReconnectsWhenDataStops) {
    IRig r;
    r.startAndHandshake();
    r.frame(true, 60, Millis(30));
    ASSERT_EQ(r.state(), InputState::Online);
    r.conn()->blackhole = true;  // link dies silently: no FIN, no data
    r.run(Millis(2000));
    EXPECT_EQ(r.state(), InputState::Online);
    EXPECT_EQ(r.in->health(r.clk.now()).level, HealthLevel::Degraded);  // frames are getting old
    r.run(Millis(1200));
    EXPECT_EQ(r.state(), InputState::Reconnecting);
    EXPECT_EQ(r.in->health(r.clk.now()).lastFailure, FailureKind::NoData);
    EXPECT_EQ(r.in->health(r.clk.now()).level, HealthLevel::Bad);
}

TEST(Input, NoDataAfterPlayTimesOut) {
    IRig r;
    r.startAndHandshake();  // handshake ok, server never sends video
    r.run(Millis(5200));
    EXPECT_EQ(r.state(), InputState::Reconnecting);
    EXPECT_EQ(r.in->health(r.clk.now()).lastFailure, FailureKind::NoData);
}

TEST(Input, RtpFlowingButNoDecodableFrameTimesOut) {
    IRig r([](RtspInputConfig& c, FakeNetwork&) { c.noFrameTimeout = Millis(2000); });
    r.startAndHandshake();
    for (int i = 0; i < 40; ++i) r.frame(false, 30, Millis(100));  // only P frames, never a keyframe
    EXPECT_EQ(r.state(), InputState::Reconnecting);
    EXPECT_NE(r.in->health(r.clk.now()).lastError.find("no decodable"), std::string::npos);
}

TEST(Input, WrongPasswordIsPermanentErrorNotARetryStorm) {
    IRig r([](RtspInputConfig& c, FakeNetwork& n) {
        n.serverCfg.auth = FakeServerConfig::Auth::Digest;
        c.url = "rtsp://admin:wrong@192.168.1.64/stream1";
    });
    r.in->start(r.clk.now());
    r.run(Millis(60000));
    EXPECT_EQ(r.net.connections.size(), 1u);
    auto h = r.in->health(r.clk.now());
    EXPECT_EQ(h.state, InputState::Error);
    EXPECT_EQ(h.lastFailure, FailureKind::AuthFailed);
}

TEST(Input, AuthFailureRetriesWhenExplicitlyAllowed) {
    IRig r([](RtspInputConfig& c, FakeNetwork& n) {
        n.serverCfg.auth = FakeServerConfig::Auth::Basic;
        c.url = "rtsp://admin:wrong@cam/stream1";
        c.retryOnAuthFailure = true;
    });
    r.in->start(r.clk.now());
    r.run(Millis(8000));
    EXPECT_GE(r.net.connections.size(), 3u);
    EXPECT_EQ(r.state(), InputState::Reconnecting);
}

TEST(Input, NotFoundKeepsRetryingWithBackoff) {
    IRig r([](RtspInputConfig& c, FakeNetwork&) { c.url = "rtsp://cam/missing"; });
    r.in->start(r.clk.now());
    r.run(Millis(8000));
    EXPECT_EQ(r.in->health(r.clk.now()).lastFailure, FailureKind::NotFound);
    EXPECT_EQ(r.net.connections.size(), 4u);  // t = 0, 1, 3, 7 s
}

TEST(Input, FollowsRedirectToAnotherHostWithoutBackoff) {
    IRig r([](RtspInputConfig&, FakeNetwork& n) {
        n.customize = [](FakeServerConfig& fc, int idx) {
            if (idx == 0) fc.redirectTo = "rtsp://10.0.0.9:8554/stream1";
        };
    });
    r.in->start(r.clk.now());
    r.run(Millis(200));
    ASSERT_EQ(r.net.connections.size(), 2u);
    EXPECT_EQ(r.net.connections[1]->target, "10.0.0.9:8554");
    EXPECT_TRUE(r.net.connections[1]->server.playing);
    EXPECT_TRUE(r.net.connections[0]->clientClosed);
    r.frame(true, 60, Millis(30));
    EXPECT_EQ(r.state(), InputState::Online);
}

TEST(Input, RedirectLoopIsBounded) {
    IRig r([](RtspInputConfig&, FakeNetwork& n) { n.serverCfg.redirectTo = "rtsp://cam/stream1"; });
    r.in->start(r.clk.now());
    r.run(Millis(500));
    EXPECT_LE(r.net.connections.size(), 7u);  // maxRedirects + the failing attempt
    EXPECT_NE(r.state(), InputState::Online);
}

TEST(Input, ConnectTimeout) {
    IRig r([](RtspInputConfig&, FakeNetwork& n) { n.connectPolls = -1; });
    r.in->start(r.clk.now());
    r.run(Millis(4900));
    EXPECT_EQ(r.state(), InputState::Connecting);
    r.run(Millis(300));
    EXPECT_EQ(r.state(), InputState::Reconnecting);
    EXPECT_EQ(r.in->health(r.clk.now()).lastFailure, FailureKind::Timeout);
}

TEST(Input, SlowTcpConnectStillSucceeds) {
    IRig r([](RtspInputConfig&, FakeNetwork& n) { n.connectPolls = 5; });
    r.startAndHandshake();
    r.frame(true, 60, Millis(30));
    EXPECT_EQ(r.state(), InputState::Online);
}

TEST(Input, SecretsNeverAppearInLogsOrHealth) {
    IRig r([](RtspInputConfig&, FakeNetwork& n) {
        n.serverCfg.auth = FakeServerConfig::Auth::Digest;
        n.refuseNext = 1;
    });
    r.startAndHandshake();
    r.run(Millis(2000));
    r.frame(true, 60, Millis(30));
    r.conn()->closeFromServer();
    r.run(Millis(1500));
    std::string logs = r.logs();
    EXPECT_FALSE(logs.empty());
    EXPECT_EQ(logs.find("s3cretPass"), std::string::npos);
    EXPECT_EQ(logs.find("admin:"), std::string::npos);
    auto h = r.in->health(r.clk.now());
    EXPECT_EQ(h.maskedUrl.find("s3cretPass"), std::string::npos);
    EXPECT_EQ(h.lastError.find("s3cretPass"), std::string::npos);
}

TEST(Input, StopSendsTeardownAndStaysStopped) {
    IRig r;
    r.startAndHandshake();
    r.frame(true, 60, Millis(30));
    auto c = r.conn();
    r.in->stop(r.clk.now());
    EXPECT_EQ(r.in->state(), InputState::Stopped);
    EXPECT_EQ(c->server.methods.back(), "TEARDOWN");
    EXPECT_TRUE(c->clientClosed);
    r.run(Millis(5000));
    EXPECT_EQ(r.net.connections.size(), 1u);  // no reconnection after stop
    EXPECT_EQ(r.in->state(), InputState::Stopped);
}

TEST(Input, RestartAfterStopWorks) {
    IRig r;
    r.startAndHandshake();
    r.in->stop(r.clk.now());
    r.in->start(r.clk.now());
    r.run(Millis(100));
    EXPECT_EQ(r.net.connections.size(), 2u);
    r.frame(true, 60, Millis(30));
    EXPECT_EQ(r.state(), InputState::Online);
}

TEST(Input, UdpTransportIsRejectedHonestly) {
    IRig r([](RtspInputConfig& c, FakeNetwork&) { c.transport = TransportMode::Udp; });
    r.in->start(r.clk.now());
    r.run(Millis(10000));
    EXPECT_EQ(r.net.connections.size(), 0u);
    auto h = r.in->health(r.clk.now());
    EXPECT_EQ(h.state, InputState::Error);
    EXPECT_EQ(h.lastFailure, FailureKind::Config);
    EXPECT_NE(h.lastError.find("not implemented"), std::string::npos);
}

TEST(Input, InvalidUrlIsPermanentConfigError) {
    IRig r([](RtspInputConfig& c, FakeNetwork&) { c.url = "http://cam/x"; });
    r.in->start(r.clk.now());
    r.run(Millis(5000));
    EXPECT_EQ(r.state(), InputState::Error);
    EXPECT_EQ(r.net.connections.size(), 0u);
}

TEST(Input, SendsReceiverReportsAndKeepalives) {
    IRig r;
    r.startAndHandshake();
    for (int i = 0; i < 70; ++i) r.frame(i % 25 == 0, 40, Millis(500));  // 35 s of video
    ASSERT_EQ(r.state(), InputState::Online);
    auto& srv = r.conn()->server;
    int rr = 0;
    for (auto& f : srv.clientFrames)
        if (f.channel == 1 && f.data.size() > 1 && f.data[1] == 201) ++rr;
    EXPECT_GE(rr, 10);  // every 2 s
    EXPECT_GE(srv.count("GET_PARAMETER"), 1);
}

TEST(Input, ServerByeTriggersReconnect) {
    IRig r;
    r.startAndHandshake();
    r.frame(true, 60, Millis(30));
    r.conn()->serverSendInterleaved(1, Bytes{0x81, 203, 0, 1, 0x11, 0x22, 0x33, 0x44});
    r.run(Millis(50));
    EXPECT_EQ(r.state(), InputState::Reconnecting);
}

TEST(Input, ThrowingConsumerCallbackDoesNotBreakTheInput) {
    IRig r;
    int calls = 0;
    r.in->setAccessUnitCallback([&](const AccessUnit&) {
        ++calls;
        throw std::runtime_error("consumer bug");
    });
    r.startAndHandshake();
    r.frame(true, 60, Millis(30));
    r.frame(false, 60, Millis(30));
    EXPECT_EQ(calls, 2);
    EXPECT_EQ(r.state(), InputState::Online);
}

TEST(Input, ThrowingStateCallbackDoesNotBreakTheInput) {
    IRig r;
    r.in->setStateCallback([](InputState, InputState, const std::string&) { throw 42; });
    r.startAndHandshake();
    r.frame(true, 60, Millis(30));
    EXPECT_EQ(r.state(), InputState::Online);
}

TEST(Input, QueueIsBoundedPerLatencyModeAndSkipsToKeyframe) {
    auto run = [](LatencyMode mode) {
        IRig r([mode](RtspInputConfig& c, FakeNetwork&) { c.latency = mode; });
        r.startAndHandshake();
        r.frame(true, 40, Millis(10));
        for (int i = 0; i < 100; ++i) r.frame(i % 50 == 49, 40, Millis(5));  // consumer never pops
        std::size_t popped = 0;
        while (r.in->popAccessUnit()) ++popped;
        return std::make_pair(popped, r.in->health(r.clk.now()).queueDropped);
    };
    auto low = run(LatencyMode::Low);
    auto stable = run(LatencyMode::Stable);
    EXPECT_LE(low.first, 8u);
    EXPECT_GT(low.second, 0u);
    EXPECT_LE(stable.first, 48u);
    EXPECT_GE(stable.first, low.first);
}

TEST(Input, SurvivesHostileServerOutput) {
    IRig r;
    r.startAndHandshake();
    r.frame(true, 60, Millis(30));
    // garbage inside valid interleaved frames and a corrupt RTSP stream afterwards
    r.conn()->serverSendInterleaved(0, Bytes{1, 2, 3});
    r.conn()->serverSendInterleaved(0, Bytes(2000, 0xAB));
    r.conn()->serverSendInterleaved(7, Bytes{1});
    r.run(Millis(50));
    EXPECT_EQ(r.state(), InputState::Online);
    r.conn()->serverSend(toBytes("\x01\x02\x03 this is not RTSP"));
    r.run(Millis(50));
    EXPECT_EQ(r.state(), InputState::Reconnecting);
    EXPECT_EQ(r.in->health(r.clk.now()).lastFailure, FailureKind::ProtocolError);
}
