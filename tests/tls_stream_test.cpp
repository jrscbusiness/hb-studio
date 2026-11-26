// TlsStream (the TLS decorator over ITcpStream) against the fake record-protocol engine and an
// in-memory pipe. No sockets, no threads: both ends are driven by hand, so every partial-read /
// partial-write / WouldBlock interleaving is deterministic.
#include <gtest/gtest.h>

#include "fake_tls.hpp"

using namespace hb::streaming;
using namespace hb::streaming::test;
using hb::rtsp::IoStatus;

namespace {

struct Pair {
    MemPair mem;
    FakeTlsEngine* clientEngine = nullptr;
    std::unique_ptr<TlsStream> client, server;
};

Pair makePair(const std::string& certName) {
    Pair p;
    p.mem = makeMemPair();
    auto ce = std::make_unique<FakeTlsEngine>(FakeTlsEngine::Role::Client);
    p.clientEngine = ce.get();
    p.client = std::make_unique<TlsStream>(std::move(p.mem.a), std::move(ce));
    p.server = std::make_unique<TlsStream>(std::move(p.mem.b),
                                           std::make_unique<FakeTlsEngine>(FakeTlsEngine::Role::Server, certName));
    return p;
}

bool handshakeBoth(Pair& p, IoStatus* clientResult = nullptr) {
    EXPECT_EQ(p.client->connect("live.example.com", 443), IoStatus::WouldBlock);   // always goes through pollConnect
    EXPECT_EQ(p.server->connect("live.example.com", 443), IoStatus::WouldBlock);
    IoStatus a = IoStatus::WouldBlock, b = IoStatus::WouldBlock;
    for (int i = 0; i < 2000; ++i) {
        if (a == IoStatus::WouldBlock) a = p.client->pollConnect();
        if (b == IoStatus::WouldBlock) b = p.server->pollConnect();
        if (a == IoStatus::Error) break;
        if (a == IoStatus::Ok && b == IoStatus::Ok) break;
    }
    if (clientResult) *clientResult = a;
    return a == IoStatus::Ok && b == IoStatus::Ok;
}

bool sendAll(TlsStream& s, const std::vector<std::uint8_t>& data) {
    std::size_t off = 0;
    for (int i = 0; i < 100000 && off < data.size(); ++i) {
        std::size_t n = 0;
        const IoStatus st = s.send(data.data() + off, data.size() - off, n);
        if (st == IoStatus::Ok) off += n;
        else if (st != IoStatus::WouldBlock) return false;
    }
    // a record may be left half-sent when the transport blocked: a zero-length send flushes it
    for (int i = 0; i < 1000; ++i) {
        std::size_t n = 0;
        if (s.send(nullptr, 0, n) == IoStatus::Ok) break;
    }
    return off == data.size();
}

std::vector<std::uint8_t> recvN(TlsStream& s, std::size_t n) {
    std::vector<std::uint8_t> out;
    std::uint8_t buf[64];
    for (int i = 0; i < 100000 && out.size() < n; ++i) {
        std::size_t got = 0;
        const IoStatus st = s.recv(buf, sizeof buf, got);
        if (st == IoStatus::Ok) out.insert(out.end(), buf, buf + got);
        else if (st != IoStatus::WouldBlock) break;
    }
    return out;
}

std::vector<std::uint8_t> pattern(std::size_t n) {
    std::vector<std::uint8_t> v(n);
    for (std::size_t i = 0; i < n; ++i) v[i] = static_cast<std::uint8_t>((i * 7 + 3) & 0xFF);
    return v;
}

}  // namespace

TEST(TlsStream, HandshakeThenBidirectionalDataRoundTrips) {
    auto p = makePair("live.example.com");
    ASSERT_TRUE(handshakeBoth(p));
    const auto data = pattern(1000);   // 10+ records at the fake's 100-byte record limit
    ASSERT_TRUE(sendAll(*p.client, data));
    EXPECT_GE(p.clientEngine->recordsOut, 10);
    EXPECT_EQ(recvN(*p.server, data.size()), data);
    const auto back = pattern(333);
    ASSERT_TRUE(sendAll(*p.server, back));
    EXPECT_EQ(recvN(*p.client, back.size()), back);
}

TEST(TlsStream, SurvivesOneByteReadsTinyWritesAndPeriodicWouldBlock) {
    auto p = makePair("live.example.com");
    p.mem.ra->maxRecvChunk = 1;   // MemStream lives inside the TlsStreams; knobs set through the raw pointers
    p.mem.rb->maxRecvChunk = 1;
    p.mem.ra->maxSendPerCall = 2;
    p.mem.rb->maxSendPerCall = 3;
    p.mem.ra->sendBlockEvery = 3;
    p.mem.rb->sendBlockEvery = 4;
    ASSERT_TRUE(handshakeBoth(p));
    const auto data = pattern(400);
    ASSERT_TRUE(sendAll(*p.client, data));
    EXPECT_EQ(recvN(*p.server, data.size()), data);
    ASSERT_TRUE(sendAll(*p.server, data));
    EXPECT_EQ(recvN(*p.client, data.size()), data);
}

TEST(TlsStream, RecvWithNothingPendingWouldBlock) {
    auto p = makePair("live.example.com");
    ASSERT_TRUE(handshakeBoth(p));
    std::uint8_t b[8];
    std::size_t got = 99;
    EXPECT_EQ(p.client->recv(b, sizeof b, got), IoStatus::WouldBlock);
    EXPECT_EQ(got, 0u);
}

TEST(TlsStream, CertificateNameMismatchFailsAndIsFlaggedAsCertificateError) {
    auto p = makePair("evil.example.net");   // server presents a certificate for another host
    IoStatus r = IoStatus::WouldBlock;
    EXPECT_FALSE(handshakeBoth(p, &r));
    EXPECT_EQ(r, IoStatus::Error);
    EXPECT_TRUE(p.client->certificateError());
    EXPECT_FALSE(p.client->lastError().empty());
    // and the channel never opens for data
    std::size_t n = 0;
    const std::uint8_t x = 1;
    EXPECT_EQ(p.client->send(&x, 1, n), IoStatus::Error);
}

TEST(TlsStream, SendBeforeHandshakeIsAnError) {
    auto p = makePair("live.example.com");
    std::size_t n = 0;
    const std::uint8_t x = 1;
    EXPECT_EQ(p.client->send(&x, 1, n), IoStatus::Error);
    std::uint8_t b[4];
    EXPECT_EQ(p.client->recv(b, sizeof b, n), IoStatus::Error);
}

TEST(TlsStream, CloseNotifyFromPeerReportsClosed) {
    auto p = makePair("live.example.com");
    ASSERT_TRUE(handshakeBoth(p));
    const auto data = pattern(50);
    ASSERT_TRUE(sendAll(*p.server, data));
    p.server->close();   // sends close_notify after the data
    EXPECT_EQ(recvN(*p.client, data.size()), data);
    std::uint8_t b[8];
    std::size_t got = 0;
    EXPECT_EQ(p.client->recv(b, sizeof b, got), IoStatus::Closed);
}

TEST(TlsStream, TransportWriteFailureSurfacesAsError) {
    auto p = makePair("live.example.com");
    ASSERT_TRUE(handshakeBoth(p));
    p.mem.ra->failSend = true;
    std::size_t n = 0;
    const std::uint8_t x = 1;
    EXPECT_EQ(p.client->send(&x, 1, n), IoStatus::Error);
}

TEST(TlsStream, ConnectWithoutEngineFailsInsteadOfFallingBackToPlaintext) {
    auto mem = makeMemPair();
    TlsStream s(std::move(mem.a), nullptr);
    EXPECT_EQ(s.connect("h", 443), IoStatus::Error);
    EXPECT_FALSE(s.lastError().empty());
}
