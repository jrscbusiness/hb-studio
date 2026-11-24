// Real-socket tests: the production RemoteServer on 127.0.0.1 against a hand-written WebSocket test
// client (own handshake/masking via the library's encoders). Validated on Linux only.
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <limits>
#include <mutex>
#include <thread>

#include "core/json.hpp"
#include "remote/server.hpp"
#include "remote/service.hpp"
#include "remote/websocket.hpp"

using namespace hb::remote;
using hb::rtsp::IoStatus;
using Clock = std::chrono::steady_clock;

namespace {

class TestClient {
public:
    bool connect(std::uint16_t port) {
        s_ = hb::rtsp::makeSocketTcpStream();
        auto st = s_->connect("127.0.0.1", port);
        const auto end = Clock::now() + std::chrono::seconds(2);
        while (st == IoStatus::WouldBlock && Clock::now() < end) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            st = s_->pollConnect();
        }
        return st == IoStatus::Ok;
    }
    void sendRaw(const std::string& s) {
        std::size_t off = 0;
        const auto end = Clock::now() + std::chrono::seconds(2);
        while (off < s.size() && Clock::now() < end) {
            std::size_t n = 0;
            auto st = s_->send(reinterpret_cast<const std::uint8_t*>(s.data()) + off, s.size() - off, n);
            if (st == IoStatus::Ok) off += n;
            else if (st == IoStatus::WouldBlock) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            else return;
        }
    }
    void sendFrame(const std::vector<std::uint8_t>& f) { sendRaw(std::string(f.begin(), f.end())); }
    // Pumps the socket for up to ms; returns false when the peer closed.
    bool pump(int ms) {
        const auto end = Clock::now() + std::chrono::milliseconds(ms);
        std::uint8_t buf[8192];
        while (Clock::now() < end) {
            std::size_t got = 0;
            auto st = s_->recv(buf, sizeof buf, got);
            if (st == IoStatus::Ok) {
                if (!upgraded) raw.append(reinterpret_cast<char*>(buf), got);
                else parser.feed(buf, got, msgs);
                if (!upgraded) {
                    const auto p = raw.find("\r\n\r\n");
                    if (p != std::string::npos && raw.compare(0, 12, "HTTP/1.1 101") == 0) {
                        upgraded = true;
                        const std::string rest = raw.substr(p + 4);
                        if (!rest.empty()) parser.feed(reinterpret_cast<const std::uint8_t*>(rest.data()), rest.size(), msgs);
                    }
                }
                continue;
            }
            if (st == IoStatus::WouldBlock) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            closed = true;
            return false;
        }
        return true;
    }
    template <class F>
    bool waitFor(F pred, int ms = 1500) {
        const auto end = Clock::now() + std::chrono::milliseconds(ms);
        while (Clock::now() < end) {
            if (pred()) return true;
            if (!pump(5) && !pred()) return false;
        }
        return pred();
    }
    bool waitClosed(int ms = 1500) {
        const auto end = Clock::now() + std::chrono::milliseconds(ms);
        while (Clock::now() < end && !closed) pump(5);
        return closed;
    }
    std::string upgradeRequest(const std::string& origin, const std::string& host, const std::string& path = "/ws") {
        std::string r = "GET " + path + " HTTP/1.1\r\nHost: " + host + "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                        "Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n";
        if (!origin.empty()) r += "Origin: " + origin + "\r\n";
        return r + "\r\n";
    }
    bool handshake(std::uint16_t port) {
        if (!s_ && !connect(port)) return false;
        const std::string host = "127.0.0.1:" + std::to_string(port);
        sendRaw(upgradeRequest("http://" + host, host));
        return waitFor([&] { return upgraded; });
    }
    void text(const std::string& s) {
        static const std::uint8_t k[4] = {1, 2, 3, 4};
        sendFrame(ws::encodeMaskedFrame(ws::Opcode::Text, reinterpret_cast<const std::uint8_t*>(s.data()), s.size(), k));
    }
    bool gotText(const std::string& needle) const {
        for (const auto& m : msgs)
            if (m.type == ws::Opcode::Text && m.data.find(needle) != std::string::npos) return true;
        return false;
    }
    std::string lastTextWith(const std::string& needle) const {
        for (auto it = msgs.rbegin(); it != msgs.rend(); ++it)
            if (it->type == ws::Opcode::Text && it->data.find(needle) != std::string::npos) return it->data;
        return {};
    }
    std::size_t binaryCount() const {
        std::size_t n = 0;
        for (const auto& m : msgs) n += m.type == ws::Opcode::Binary;
        return n;
    }
    bool gotClose(std::uint16_t code) const {
        for (const auto& m : msgs)
            if (m.type == ws::Opcode::Close && m.closeCode == code) return true;
        return false;
    }
    std::string pairAndGetToken(AuthManager& auth, std::uint16_t port, const std::string& name = "Teste") {
        if (!handshake(port)) return {};
        text("{\"t\":\"pair\",\"pin\":\"" + auth.pin() + "\",\"name\":\"" + name + "\"}");
        if (!waitFor([&] { return gotText("paired"); })) return {};
        hb::core::json::Value v;
        hb::core::json::parse(lastTextWith("paired"), v);
        return v.get("token") ? v.get("token")->s : std::string();
    }

    std::unique_ptr<hb::rtsp::ITcpStream> s_;
    std::string raw;
    bool upgraded = false, closed = false;
    ws::Parser parser{ws::ParserLimits{1 << 20}, false};
    std::vector<ws::Message> msgs;
};

struct Fixture {
    AuthManager auth{systemRandom()};
    std::mutex mu;
    std::vector<std::string> cmds;
    std::unique_ptr<RemoteServer> srv;
    explicit Fixture(ServerConfig cfg = {}) {
        cfg.pageHtml = "<html>page</html>";
        srv = std::make_unique<RemoteServer>(cfg, auth, [this](const std::string& c) {
            std::lock_guard l(mu);
            cmds.push_back(c);
        });
        std::string err;
        EXPECT_TRUE(srv->start(err)) << err;
    }
    ~Fixture() { srv->stop(); }
    std::vector<std::string> commands() {
        std::lock_guard l(mu);
        return cmds;
    }
    bool waitCommands(std::size_t n, int ms = 1500) {
        const auto end = Clock::now() + std::chrono::milliseconds(ms);
        while (Clock::now() < end) {
            if (commands().size() >= n) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return false;
    }
    std::uint16_t port() const { return srv->port(); }
};
}  // namespace

TEST(RemoteServer, ServesPageWithSecurityHeaders) {
    Fixture f;
    TestClient c;
    ASSERT_TRUE(c.connect(f.port()));
    c.sendRaw("GET / HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(f.port()) + "\r\n\r\n");
    ASSERT_TRUE(c.waitFor([&] { return c.raw.find("</html>") != std::string::npos; }));
    EXPECT_EQ(c.raw.compare(0, 15, "HTTP/1.1 200 OK"), 0);
    EXPECT_NE(c.raw.find("Content-Security-Policy"), std::string::npos);
    EXPECT_NE(c.raw.find("X-Frame-Options: DENY"), std::string::npos);
    EXPECT_NE(c.raw.find("Cache-Control: no-store"), std::string::npos);
    EXPECT_TRUE(c.waitClosed());
}

TEST(RemoteServer, RejectsBadHostMethodPath) {
    Fixture f;
    auto status = [&](const std::string& req) {
        TestClient c;
        EXPECT_TRUE(c.connect(f.port()));
        c.sendRaw(req);
        c.waitClosed();
        return c.raw.substr(0, 12);
    };
    const std::string h = "127.0.0.1:" + std::to_string(f.port());
    EXPECT_EQ(status("GET / HTTP/1.1\r\nHost: evil.example.com\r\n\r\n"), "HTTP/1.1 403");  // DNS rebinding
    EXPECT_EQ(status("GET / HTTP/1.1\r\n\r\n"), "HTTP/1.1 403");                            // no Host
    EXPECT_EQ(status("POST / HTTP/1.1\r\nHost: " + h + "\r\n\r\n"), "HTTP/1.1 405");
    EXPECT_EQ(status("GET /admin HTTP/1.1\r\nHost: " + h + "\r\n\r\n"), "HTTP/1.1 404");
    EXPECT_EQ(status("garbage\r\n\r\n"), "HTTP/1.1 400");
    EXPECT_EQ(status("GET /ws HTTP/1.1\r\nHost: " + h + "\r\n\r\n"), "HTTP/1.1 426");
}

TEST(RemoteServer, WebSocketOriginAndHandshakeChecks) {
    Fixture f;
    const std::string h = "127.0.0.1:" + std::to_string(f.port());
    auto status = [&](const std::string& req) {
        TestClient c;
        EXPECT_TRUE(c.connect(f.port()));
        c.sendRaw(req);
        c.pump(150);
        return c.raw.substr(0, 12);
    };
    TestClient t;
    EXPECT_EQ(status(t.upgradeRequest("http://evil.com", h)), "HTTP/1.1 403");   // cross-site WebSocket hijacking
    EXPECT_EQ(status(t.upgradeRequest("", h)), "HTTP/1.1 403");                   // no Origin
    EXPECT_EQ(status(t.upgradeRequest("http://" + h, "evil.com")), "HTTP/1.1 403");
    EXPECT_EQ(status(t.upgradeRequest("http://" + h, h)), "HTTP/1.1 101");
    TestClient c;
    ASSERT_TRUE(c.connect(f.port()));
    c.sendRaw(t.upgradeRequest("http://" + h, h));
    c.pump(200);
    EXPECT_NE(c.raw.find("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo="), std::string::npos);
}

TEST(RemoteServer, PairingCommandsStateAndReconnectWithToken) {
    Fixture f;
    TestClient c;
    const std::string token = c.pairAndGetToken(f.auth, f.port(), "iPad");
    ASSERT_EQ(token.size(), 64u);
    ASSERT_TRUE(c.waitFor([&] { return c.gotText("hello"); }));

    f.srv->broadcastState("{\"t\":\"s\",\"pvw\":1}");
    ASSERT_TRUE(c.waitFor([&] { return c.gotText("\"pvw\":1"); }));

    c.text(R"({"t":"cmd","c":"cut"})");
    c.text(R"({"t":"cmd","c":"fader","ch":1,"db":-3})");
    c.text(R"({"t":"cmd","c":"xmitremove"})");  // not allowed
    ASSERT_TRUE(f.waitCommands(2));
    ASSERT_TRUE(c.waitFor([&] { return c.gotText("not_allowed"); }));
    auto cmds = f.commands();
    ASSERT_EQ(cmds.size(), 2u);
    EXPECT_EQ(cmds[0], "cut");
    EXPECT_EQ(cmds[1], "aud|strip|1|faderDb|-3.0");

    c.text(R"({"t":"p"})");
    ASSERT_TRUE(c.waitFor([&] { return c.gotText("pong"); }));
    auto live = f.srv->connectedDevices();
    ASSERT_EQ(live.size(), 1u);

    // A second connection logs in with the token only.
    TestClient d;
    ASSERT_TRUE(d.handshake(f.port()));
    d.text("{\"t\":\"auth\",\"token\":\"" + token + "\"}");
    ASSERT_TRUE(d.waitFor([&] { return d.gotText("hello"); }));
    ASSERT_TRUE(d.waitFor([&] { return d.gotText("\"pvw\":1"); }));  // late joiner gets the current state at once
    // Wrong token -> authfail + close 1008.
    TestClient e;
    ASSERT_TRUE(e.handshake(f.port()));
    e.text("{\"t\":\"auth\",\"token\":\"" + std::string(64, '0') + "\"}");
    ASSERT_TRUE(e.waitFor([&] { return e.gotText("authfail"); }));
    EXPECT_TRUE(e.waitFor([&] { return e.gotClose(1008); }));
}

TEST(RemoteServer, StateIsDedupedAndRateLimited) {
    ServerConfig cfg;
    cfg.stateMinIntervalMs = 50;
    Fixture f(cfg);
    TestClient c;
    ASSERT_FALSE(c.pairAndGetToken(f.auth, f.port()).empty());
    c.pump(100);
    const auto before = c.msgs.size();
    for (int i = 0; i < 20; ++i) f.srv->broadcastState("{\"t\":\"s\",\"n\":0}");  // identical: one push at most
    c.pump(300);
    std::size_t states = 0;
    for (std::size_t i = before; i < c.msgs.size(); ++i) states += c.msgs[i].data.find("\"n\":0") != std::string::npos;
    EXPECT_EQ(states, 1u);
    // 200 distinct states in ~100 ms must be coalesced to far fewer messages.
    const auto mark = c.msgs.size();
    for (int i = 0; i < 200; ++i) {
        f.srv->broadcastState("{\"t\":\"s\",\"n\":" + std::to_string(i + 1) + "}");
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
    c.pump(300);
    EXPECT_LT(c.msgs.size() - mark, 15u);
    EXPECT_TRUE(c.gotText("\"n\":200"));  // but the latest one always arrives
}

TEST(RemoteServer, UnauthenticatedClientCannotDoAnything) {
    Fixture f;
    TestClient c;
    ASSERT_TRUE(c.handshake(f.port()));
    c.text(R"({"t":"cmd","c":"cut"})");
    EXPECT_TRUE(c.waitFor([&] { return c.gotClose(1008); }));
    EXPECT_TRUE(c.waitClosed());
    EXPECT_TRUE(f.commands().empty());
}

TEST(RemoteServer, WrongPinsLockOut) {
    Fixture f;
    const std::string good = f.auth.pin();
    TestClient c;
    ASSERT_TRUE(c.handshake(f.port()));
    for (int i = 0; i < 3; ++i) c.text(R"({"t":"pair","pin":"abcdef"})");
    ASSERT_TRUE(c.waitFor([&] { return c.gotClose(1008); }));  // 3 attempts per connection
    for (int k = 0; k < 2; ++k) {                              // reach the global threshold (5 failures)
        TestClient d;
        ASSERT_TRUE(d.handshake(f.port()));
        d.text(R"({"t":"pair","pin":"abcdef"})");
        d.text(R"({"t":"pair","pin":"abcdef"})");
        d.waitFor([&] { return d.gotText("left"); });
        d.pump(100);
    }
    EXPECT_GT(f.auth.lockedFor(Clock::now()).count(), 0);
    EXPECT_NE(f.auth.pin(), good);
    TestClient e;
    ASSERT_TRUE(e.handshake(f.port()));
    e.text("{\"t\":\"pair\",\"pin\":\"" + f.auth.pin() + "\"}");  // right PIN, still refused
    EXPECT_TRUE(e.waitFor([&] { return e.gotText("locked"); }));
}

TEST(RemoteServer, RevokeAndKickDisconnect) {
    Fixture f;
    TestClient a, b;
    const std::string ta = a.pairAndGetToken(f.auth, f.port(), "A");
    const std::string tb = b.pairAndGetToken(f.auth, f.port(), "B");
    ASSERT_FALSE(ta.empty());
    ASSERT_FALSE(tb.empty());
    auto sessions = f.auth.sessions(Clock::now());
    ASSERT_EQ(sessions.size(), 2u);
    f.auth.revoke(sessions[0].id);  // kick
    EXPECT_TRUE(a.waitFor([&] { return a.gotClose(1008); }));
    EXPECT_FALSE(b.closed);
    f.auth.revokeAll();
    EXPECT_TRUE(b.waitFor([&] { return b.gotClose(1008); }));
    EXPECT_TRUE(b.waitClosed());
}

TEST(RemoteServer, ProtocolAbuseClosesConnection) {
    Fixture f;
    {
        TestClient c;
        ASSERT_FALSE(c.pairAndGetToken(f.auth, f.port()).empty());
        c.text("{\"t\":\"cmd\",\"c\":\"cut\",\"pad\":\"" + std::string(5000, 'a') + "\"}");  // > maxInboundMessageBytes
        EXPECT_TRUE(c.waitFor([&] { return c.gotClose(1009); }));
    }
    {
        TestClient c;
        ASSERT_FALSE(c.pairAndGetToken(f.auth, f.port()).empty());
        c.sendRaw("\x81\x02hi");  // unmasked frame from a client
        EXPECT_TRUE(c.waitFor([&] { return c.gotClose(1002); }));
    }
    {
        TestClient c;
        ASSERT_FALSE(c.pairAndGetToken(f.auth, f.port()).empty());
        c.text("not json at all");
        EXPECT_TRUE(c.waitFor([&] { return c.gotClose(1008); }));
    }
    EXPECT_TRUE(f.commands().empty());
}

TEST(RemoteServer, ClientCloseIsAnswered) {
    Fixture f;
    TestClient c;
    ASSERT_FALSE(c.pairAndGetToken(f.auth, f.port()).empty());
    static const std::uint8_t k[4] = {9, 9, 9, 9};
    std::uint8_t pl[2] = {0x03, 0xE8};
    c.sendFrame(ws::encodeMaskedFrame(ws::Opcode::Close, pl, 2, k));
    EXPECT_TRUE(c.waitFor([&] { return c.gotClose(1000); }));
    EXPECT_TRUE(c.waitClosed());
}

TEST(RemoteServer, CommandRateLimit) {
    ServerConfig cfg;
    cfg.commandsPerSec = 5;
    Fixture f(cfg);
    TestClient c;
    ASSERT_FALSE(c.pairAndGetToken(f.auth, f.port()).empty());
    for (int i = 0; i < 30; ++i) c.text(R"({"t":"cmd","c":"cut"})");
    ASSERT_TRUE(c.waitFor([&] { return c.gotText("rate_limited"); }));
    c.pump(200);
    EXPECT_LE(f.commands().size(), 8u);
}

TEST(RemoteServer, AuthTimeoutAndSlowlorisHttp) {
    ServerConfig cfg;
    cfg.authTimeoutMs = 150;
    cfg.httpTimeoutMs = 150;
    Fixture f(cfg);
    TestClient slow;
    ASSERT_TRUE(slow.connect(f.port()));
    slow.sendRaw("GET / HTTP/1.1\r\nHost: 127.0.0.1\r\n");  // never finishes
    EXPECT_TRUE(slow.waitClosed(1000));
    TestClient idle;
    ASSERT_TRUE(idle.handshake(f.port()));  // upgraded but silent
    EXPECT_TRUE(idle.waitFor([&] { return idle.gotClose(1008); }));
}

TEST(RemoteServer, ThumbnailsAreOptInAndBandwidthCapped) {
    ServerConfig cfg;
    cfg.thumbBytesPerSec = 20000;
    Fixture f(cfg);
    TestClient c;
    ASSERT_FALSE(c.pairAndGetToken(f.auth, f.port()).empty());
    std::vector<std::uint8_t> jpeg(8000, 0xAB);
    EXPECT_FALSE(f.srv->wantsThumbs());
    f.srv->broadcastThumb(1, jpeg);  // nobody opted in: ignored
    c.pump(100);
    EXPECT_EQ(c.binaryCount(), 0u);
    c.text(R"({"t":"thumbs","on":true})");
    ASSERT_TRUE(c.waitFor([&] { return f.srv->wantsThumbs(); }));
    const auto start = Clock::now();
    while (Clock::now() - start < std::chrono::milliseconds(700)) {
        f.srv->broadcastThumb(0, jpeg);
        f.srv->broadcastThumb(1, jpeg);
        c.pump(5);
    }
    ASSERT_GT(c.binaryCount(), 0u);
    // 20 KB/s over ~0.7 s (+1 s burst bucket, + one frame of debt) is at most ~ 40 KB -> <= 6 frames of 8 KB.
    EXPECT_LE(c.binaryCount(), 6u);
    bool sawKind0 = false, sawKind1 = false;
    for (const auto& m : c.msgs)
        if (m.type == ws::Opcode::Binary && m.data.size() == 8001) {
            sawKind0 |= m.data[0] == 0;
            sawKind1 |= m.data[0] == 1;
            EXPECT_EQ(static_cast<std::uint8_t>(m.data[1]), 0xAB);
        }
    EXPECT_TRUE(sawKind0 || sawKind1);
}

TEST(RemoteServer, SlowClientNeverStallsFeederNorOtherClients) {
    ServerConfig cfg;
    cfg.stallTimeoutMs = 400;
    cfg.thumbBytesPerSec = 1 << 30;
    cfg.maxThumbBytes = 64 * 1024;
    Fixture f(cfg);
    TestClient slow, fast;
    ASSERT_FALSE(slow.pairAndGetToken(f.auth, f.port(), "slow").empty());
    ASSERT_FALSE(fast.pairAndGetToken(f.auth, f.port(), "fast").empty());
    slow.text(R"({"t":"thumbs","on":true})");
    fast.text(R"({"t":"thumbs","on":true})");
    ASSERT_TRUE(fast.waitFor([&] { return f.srv->wantsThumbs(); }));
    slow.pump(50);  // from now on `slow` never reads again
    std::vector<std::uint8_t> big(60000, 0x55);
    long long worstUs = 0;
    const auto start = Clock::now();
    int i = 0;
    while (Clock::now() - start < std::chrono::milliseconds(1800) && f.srv->droppedClients() == 0) {
        const auto t0 = Clock::now();
        f.srv->broadcastThumb(i & 1, big);
        f.srv->broadcastState("{\"t\":\"s\",\"i\":" + std::to_string(i) + "}");
        worstUs = std::max<long long>(worstUs, std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t0).count());
        ++i;
        fast.pump(2);  // the healthy client keeps draining
    }
    EXPECT_LT(worstUs, 50000) << "feeder-side broadcast must never block on a slow client";
    EXPECT_GE(f.srv->droppedClients(), 1u) << "hopeless client must be disconnected";
    // The fast client is still alive and still receives fresh state.
    f.srv->broadcastState("{\"t\":\"s\",\"final\":true}");
    EXPECT_TRUE(fast.waitFor([&] { return fast.gotText("final"); }));
}

TEST(RemoteServer, ConnectionLimits) {
    ServerConfig cfg;
    cfg.maxPerIp = 3;
    Fixture f(cfg);
    std::vector<std::unique_ptr<TestClient>> cs;
    for (int i = 0; i < 3; ++i) {
        cs.push_back(std::make_unique<TestClient>());
        ASSERT_TRUE(cs.back()->connect(f.port()));
        cs.back()->pump(20);
    }
    TestClient extra;
    ASSERT_TRUE(extra.connect(f.port()));
    extra.pump(300);
    EXPECT_EQ(extra.raw.substr(0, 12), "HTTP/1.1 503");
}

TEST(RemoteServer, StopDisconnectsEveryoneAndIsIdempotent) {
    Fixture f;
    TestClient c;
    ASSERT_FALSE(c.pairAndGetToken(f.auth, f.port()).empty());
    f.srv->stop();
    EXPECT_TRUE(c.waitClosed());
    f.srv->stop();
    EXPECT_FALSE(f.srv->running());
}

TEST(RemoteService, EnableDisableLifecycleAndStatus) {
    std::mutex m;
    std::vector<std::string> cmds;
    ServiceOptions opt;
    opt.bindOverride = {"127.0.0.1"};
    opt.preferredPort = 0;
    RemoteService svc([&](const std::string& c) { std::lock_guard l(m); cmds.push_back(c); }, {}, opt);
    EXPECT_FALSE(svc.enabled());  // OFF by default
    {
        const std::string off = svc.statusJson();
        EXPECT_NE(off.find("\"enabled\":false"), std::string::npos);
        EXPECT_EQ(off.find("pin"), std::string::npos);  // PIN is never exposed while off
    }
    std::string err;
    ASSERT_TRUE(svc.enable(err)) << err;
    hb::core::json::Value v;
    ASSERT_TRUE(hb::core::json::parse(svc.statusJson(), v));
    EXPECT_TRUE(v.get("enabled")->b);
    EXPECT_EQ(v.get("pin")->s.size(), 6u);
    ASSERT_EQ(v.get("urls")->a.size(), 1u);
    EXPECT_EQ(v.get("urls")->a[0].s, "http://127.0.0.1:" + std::to_string(svc.port()) + "/");

    TestClient c;
    const std::string token = c.pairAndGetToken(svc.auth(), svc.port(), "Phone");
    ASSERT_FALSE(token.empty());
    svc.pushState(RemoteState{});
    ASSERT_TRUE(c.waitFor([&] { return c.gotText("\"t\":\"s\""); }));
    ASSERT_TRUE(hb::core::json::parse(svc.statusJson(), v));
    ASSERT_EQ(v.get("devices")->a.size(), 1u);
    EXPECT_EQ(v.get("devices")->a[0].get("name")->s, "Phone");
    EXPECT_TRUE(v.get("devices")->a[0].get("online")->b);
    EXPECT_EQ(v.get("devices")->a[0].get("ip")->s, "127.0.0.1");

    svc.handleUi("remote|kick|" + std::to_string(static_cast<long long>(v.get("devices")->a[0].get("id")->n)));
    EXPECT_TRUE(c.waitFor([&] { return c.gotClose(1008); }));

    svc.handleUi("remote|off");
    EXPECT_FALSE(svc.enabled());
    TestClient d;
    EXPECT_FALSE(d.connect(svc.port() ? svc.port() : 1) && d.handshake(svc.port() ? svc.port() : 1));
    EXPECT_EQ(svc.handleUi("remote|bogus|x"), "");
    EXPECT_EQ(svc.handleUi("remote|kick|abc"), "");
}

TEST(RemoteService, NoLanReportsHonestError) {
    ServiceOptions opt;
    opt.bindOverride = {"203.0.113.77"};  // not a local address: bind must fail cleanly, not crash
    opt.preferredPort = 0;
    RemoteService svc([](const std::string&) {}, {}, opt);
    std::string err;
    EXPECT_FALSE(svc.enable(err));
    EXPECT_FALSE(err.empty());
    EXPECT_FALSE(svc.enabled());
}
