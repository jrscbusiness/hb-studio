// Real-socket tests over 127.0.0.1: the scripted RTSP server runs in a thread behind a real
// TcpListener, the client side is the production RtspInput + socket ITcpStream.
// Validated on Linux (POSIX sockets). The Winsock implementation compiles from the same tests
// but has not been run on Windows.
#include <gtest/gtest.h>

#include <atomic>
#include <thread>

#include "rtsp/rtsp_input.hpp"
#include "rtsp_test_support.hpp"

using namespace hb::rtsp;
using namespace hb::rtsp::test;

namespace {
bool sendAll(ITcpStream& s, const Bytes& b, std::atomic<bool>& stop) {
    std::size_t off = 0;
    while (off < b.size() && !stop) {
        std::size_t n = 0;
        IoStatus st = s.send(b.data() + off, b.size() - off, n);
        if (st == IoStatus::Ok) off += n;
        else if (st == IoStatus::WouldBlock) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        else return false;
    }
    return off == b.size();
}

// Serves one RTSP client until it disconnects, `stop` is set, or `maxMs` elapsed.
// After PLAY it streams a keyframe followed by P frames every 10 ms.
void serve(ITcpStream& s, std::atomic<bool>& stop, int maxMs, std::atomic<int>& framesSent) {
    FakeRtspServer srv;
    Packetizer pk;
    pk.mtu = 1200;
    std::uint32_t ts = 0;
    int n = 0;
    auto start = std::chrono::steady_clock::now();
    auto nextFrame = start;
    Bytes buf(4096);
    while (!stop && std::chrono::steady_clock::now() - start < std::chrono::milliseconds(maxMs)) {
        std::size_t got = 0;
        IoStatus st = s.recv(buf.data(), buf.size(), got);
        if (st == IoStatus::Ok) {
            Bytes resp = srv.onBytes(ByteView(buf.data(), got));
            if (!resp.empty() && !sendAll(s, resp, stop)) return;
        } else if (st == IoStatus::Closed || st == IoStatus::Error) {
            return;
        }
        if (srv.playing && std::chrono::steady_clock::now() >= nextFrame) {
            ts += 3600;
            auto pkts = pk.h264({n % 30 == 0 ? idr264(3000, static_cast<std::uint8_t>(n)) : p264(500, static_cast<std::uint8_t>(n))}, ts);
            for (auto& p : pkts)
                if (!sendAll(s, frameInterleaved(0, p), stop)) return;
            ++n;
            ++framesSent;
            nextFrame += std::chrono::milliseconds(10);
        }
        if (st == IoStatus::WouldBlock) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

bool waitFor(const std::function<bool()>& cond, int ms) {
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        if (cond()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return cond();
}
}  // namespace

TEST(Loopback, ListenerAndStreamExchangeBytes) {
    auto l = makeLoopbackListener();
    ASSERT_TRUE(l);
    ASSERT_NE(l->port(), 0);
    auto c = makeSocketTcpStream();
    IoStatus st = c->connect("127.0.0.1", l->port());
    ASSERT_NE(st, IoStatus::Error) << c->lastError();
    auto srv = l->accept(2000);
    ASSERT_TRUE(srv);
    for (int i = 0; i < 200 && c->pollConnect() == IoStatus::WouldBlock; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    ASSERT_EQ(c->pollConnect(), IoStatus::Ok);
    std::size_t sent = 0;
    const char* msg = "hello";
    ASSERT_EQ(c->send(reinterpret_cast<const std::uint8_t*>(msg), 5, sent), IoStatus::Ok);
    EXPECT_EQ(sent, 5u);
    std::uint8_t buf[16];
    std::size_t got = 0;
    ASSERT_TRUE(srv->waitReadable(1000));
    ASSERT_EQ(srv->recv(buf, sizeof buf, got), IoStatus::Ok);
    EXPECT_EQ(std::string(reinterpret_cast<char*>(buf), got), "hello");
    EXPECT_EQ(c->recv(buf, sizeof buf, got), IoStatus::WouldBlock);
    srv->close();
    ASSERT_TRUE(c->waitReadable(1000));
    EXPECT_EQ(c->recv(buf, sizeof buf, got), IoStatus::Closed);
}

TEST(Loopback, ConnectionRefusedIsReportedAsError) {
    std::uint16_t port;
    {
        auto l = makeLoopbackListener();
        ASSERT_TRUE(l);
        port = l->port();
    }  // listener closed: nobody is listening any more
    auto c = makeSocketTcpStream();
    IoStatus st = c->connect("127.0.0.1", port);
    for (int i = 0; i < 500 && st == IoStatus::WouldBlock; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        st = c->pollConnect();
    }
    EXPECT_EQ(st, IoStatus::Error);
    EXPECT_FALSE(c->lastError().empty());
}

TEST(Loopback, RtspInputStreamsOverRealTcpAndReconnects) {
    auto listener = makeLoopbackListener();
    ASSERT_TRUE(listener);
    std::atomic<bool> stop{false};
    std::atomic<int> frames{0};
    std::thread server([&] {
        // 1st connection: serve ~300 ms of video then hang up. 2nd connection: serve until stop.
        if (auto s = listener->accept(3000)) serve(*s, stop, 300, frames);
        if (auto s = listener->accept(5000)) serve(*s, stop, 10000, frames);
    });

    RtspInputConfig cfg;
    cfg.url = "rtsp://127.0.0.1:" + std::to_string(listener->port()) + "/stream1";
    cfg.reconnectInitial = Millis(100);
    cfg.latency = hb::core::LatencyMode::Low;
    RtspInput in(cfg);
    in.setAccessUnitCallback(nullptr);

    std::atomic<int> aus{0};
    std::atomic<int> keys{0};
    std::atomic<int> reconnecting{0};
    in.setAccessUnitCallback([&](const AccessUnit& au) {
        ++aus;
        if (au.keyframe) ++keys;
    });
    in.setStateCallback([&](InputState, InputState to, const std::string&) {
        if (to == InputState::Reconnecting) ++reconnecting;
    });
    in.startThread();

    EXPECT_TRUE(waitFor([&] { return aus >= 10; }, 3000)) << "frames received: " << aus;
    EXPECT_GE(keys, 1);
    EXPECT_EQ(in.state(), InputState::Online);
    // The server hangs up after ~300 ms: the input must notice and reconnect by itself.
    EXPECT_TRUE(waitFor([&] { return reconnecting >= 1; }, 3000));
    int before = aus;
    EXPECT_TRUE(waitFor([&] { return aus >= before + 10 && in.state() == InputState::Online; }, 4000));
    auto h = in.health(std::chrono::steady_clock::now());
    EXPECT_GE(h.reconnects, 1u);
    EXPECT_EQ(h.level, HealthLevel::Good);

    in.stopThread();
    EXPECT_EQ(in.state(), InputState::Stopped);
    stop = true;
    server.join();
}
