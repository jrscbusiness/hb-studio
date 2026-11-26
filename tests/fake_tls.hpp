#pragma once
// Test doubles for the TLS layer: a trivially "encrypted" record protocol (NOT secure, never shipped)
// that nevertheless exercises everything TlsStream must get right - multi-flight handshake, record
// framing across arbitrary read boundaries, partial writes, WouldBlock, certificate-name rejection -
// plus an in-memory ITcpStream pair whose chunking/blocking behaviour the tests control.
#include <algorithm>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "rtsp/tcp_stream.hpp"
#include "streaming/tls_engine.hpp"
#include "streaming/tls_stream.hpp"

namespace hb::streaming::test {

// Record: [type u8][len u16 BE][payload]. 0x01 ClientHello(serverName) -> 0x02 ServerHello(certName)
// -> 0x03 ClientFinished -> 0x04 ServerFinished. 0x17 = application data (payload XOR 0x5A), 0x15 = close.
class FakeTlsEngine final : public ITlsEngine {
public:
    enum class Role { Client, Server };
    FakeTlsEngine(Role role, std::string certName = {}, std::size_t maxPlain = 100)
        : role_(role), certName_(std::move(certName)), maxPlain_(maxPlain) {}

    bool begin(const std::string& serverName) override {
        serverName_ = serverName;
        step_ = 0;
        return true;
    }

    TlsStatus handshake(std::vector<std::uint8_t>& in, std::vector<std::uint8_t>& out) override {
        if (role_ == Role::Client) {
            if (step_ == 0) {
                put(out, 0x01, std::vector<std::uint8_t>(serverName_.begin(), serverName_.end()));
                step_ = 1;
                return TlsStatus::NeedMoreInput;
            }
            std::uint8_t type = 0;
            std::vector<std::uint8_t> payload;
            if (!take(in, type, payload)) return TlsStatus::NeedMoreInput;
            if (step_ == 1) {
                if (type != 0x02) return fail("flight inesperado");
                const std::string cert(payload.begin(), payload.end());
                if (cert != serverName_) {
                    certErr_ = true;
                    return fail("o certificado do servidor não corresponde ao nome do servidor.");
                }
                put(out, 0x03, {});
                step_ = 2;
                return TlsStatus::NeedMoreInput;
            }
            if (type != 0x04) return fail("flight inesperado");
            step_ = 3;
            return TlsStatus::Ok;
        }
        // server
        std::uint8_t type = 0;
        std::vector<std::uint8_t> payload;
        if (!take(in, type, payload)) return TlsStatus::NeedMoreInput;
        if (step_ == 0 && type == 0x01) {
            put(out, 0x02, std::vector<std::uint8_t>(certName_.begin(), certName_.end()));
            step_ = 1;
            return in.empty() ? TlsStatus::NeedMoreInput : handshake(in, out);
        }
        if (step_ == 1 && type == 0x03) {
            put(out, 0x04, {});
            step_ = 3;
            return TlsStatus::Ok;
        }
        return fail("flight inesperado");
    }

    std::size_t maxPlaintext() const override { return maxPlain_; }

    TlsStatus encrypt(const std::uint8_t* p, std::size_t n, std::vector<std::uint8_t>& out) override {
        if (n > maxPlain_) return fail("registro grande demais");
        std::vector<std::uint8_t> x(p, p + n);
        for (auto& b : x) b ^= 0x5A;
        put(out, 0x17, x);
        ++recordsOut;
        return TlsStatus::Ok;
    }

    TlsStatus decrypt(std::vector<std::uint8_t>& in, std::vector<std::uint8_t>& plain, std::vector<std::uint8_t>&) override {
        std::uint8_t type = 0;
        std::vector<std::uint8_t> payload;
        if (!take(in, type, payload)) return TlsStatus::NeedMoreInput;
        if (type == 0x15) return TlsStatus::Closed;
        if (type != 0x17) return fail("registro inesperado");
        for (auto b : payload) plain.push_back(static_cast<std::uint8_t>(b ^ 0x5A));
        return TlsStatus::Ok;
    }

    void shutdown(std::vector<std::uint8_t>& out) override { put(out, 0x15, {}); }
    std::string lastError() const override { return err_; }
    bool certificateError() const override { return certErr_; }

    int recordsOut = 0;

private:
    static void put(std::vector<std::uint8_t>& out, std::uint8_t type, const std::vector<std::uint8_t>& payload) {
        out.push_back(type);
        out.push_back(static_cast<std::uint8_t>(payload.size() >> 8));
        out.push_back(static_cast<std::uint8_t>(payload.size() & 0xFF));
        out.insert(out.end(), payload.begin(), payload.end());
    }
    static bool take(std::vector<std::uint8_t>& in, std::uint8_t& type, std::vector<std::uint8_t>& payload) {
        if (in.size() < 3) return false;
        const std::size_t len = (static_cast<std::size_t>(in[1]) << 8) | in[2];
        if (in.size() < 3 + len) return false;
        type = in[0];
        payload.assign(in.begin() + 3, in.begin() + 3 + static_cast<std::ptrdiff_t>(len));
        in.erase(in.begin(), in.begin() + 3 + static_cast<std::ptrdiff_t>(len));
        return true;
    }
    TlsStatus fail(const std::string& m) {
        err_ = m;
        return TlsStatus::Error;
    }

    Role role_;
    std::string certName_, serverName_, err_;
    std::size_t maxPlain_;
    int step_ = 0;
    bool certErr_ = false;
};

// One direction of an in-memory duplex pipe.
struct PipeDir {
    std::deque<std::uint8_t> bytes;
    bool closed = false;
};

// Endpoint of an in-memory ITcpStream pair with controllable behaviour.
class MemStream final : public hb::rtsp::ITcpStream {
public:
    MemStream(std::shared_ptr<PipeDir> rx, std::shared_ptr<PipeDir> tx) : rx_(std::move(rx)), tx_(std::move(tx)) {}

    // test knobs
    std::size_t maxRecvChunk = 1 << 20;     // bytes returned per recv()
    std::size_t maxSendPerCall = 1 << 20;   // bytes accepted per send()
    int sendBlockEvery = 0;                 // every Nth send() call returns WouldBlock (0 = never)
    bool failSend = false;
    int sendCalls = 0;

    hb::rtsp::IoStatus connect(const std::string&, std::uint16_t) override { return hb::rtsp::IoStatus::Ok; }
    hb::rtsp::IoStatus pollConnect() override { return hb::rtsp::IoStatus::Ok; }
    hb::rtsp::IoStatus send(const std::uint8_t* d, std::size_t len, std::size_t& sent) override {
        sent = 0;
        ++sendCalls;
        if (failSend) return hb::rtsp::IoStatus::Error;
        if (sendBlockEvery > 0 && sendCalls % sendBlockEvery == 0) return hb::rtsp::IoStatus::WouldBlock;
        const std::size_t n = std::min(len, maxSendPerCall);
        tx_->bytes.insert(tx_->bytes.end(), d, d + n);
        sent = n;
        return hb::rtsp::IoStatus::Ok;
    }
    hb::rtsp::IoStatus recv(std::uint8_t* buf, std::size_t cap, std::size_t& got) override {
        got = 0;
        if (rx_->bytes.empty()) return rx_->closed ? hb::rtsp::IoStatus::Closed : hb::rtsp::IoStatus::WouldBlock;
        const std::size_t n = std::min({cap, rx_->bytes.size(), maxRecvChunk});
        for (std::size_t i = 0; i < n; ++i) {
            buf[i] = rx_->bytes.front();
            rx_->bytes.pop_front();
        }
        got = n;
        return hb::rtsp::IoStatus::Ok;
    }
    bool waitReadable(int) override { return !rx_->bytes.empty(); }
    void close() override { tx_->closed = true; }
    std::string lastError() const override { return "mem error"; }

private:
    std::shared_ptr<PipeDir> rx_, tx_;
};

struct MemPair {
    std::unique_ptr<MemStream> a, b;
    MemStream *ra = nullptr, *rb = nullptr;   // raw access for knobs after ownership moves
};
inline MemPair makeMemPair() {
    auto ab = std::make_shared<PipeDir>(), ba = std::make_shared<PipeDir>();
    MemPair p;
    p.a = std::make_unique<MemStream>(ba, ab);
    p.b = std::make_unique<MemStream>(ab, ba);
    p.ra = p.a.get();
    p.rb = p.b.get();
    return p;
}

// Wraps an ALREADY connected stream (e.g. an accepted socket) so a TlsStream can sit on top of it in
// the server role: connect() is a no-op that reports success, everything else passes through.
class ConnectedPassthrough final : public hb::rtsp::ITcpStream {
public:
    explicit ConnectedPassthrough(std::unique_ptr<hb::rtsp::ITcpStream> in) : in_(std::move(in)) {}
    hb::rtsp::IoStatus connect(const std::string&, std::uint16_t) override { return hb::rtsp::IoStatus::Ok; }
    hb::rtsp::IoStatus pollConnect() override { return hb::rtsp::IoStatus::Ok; }
    hb::rtsp::IoStatus send(const std::uint8_t* d, std::size_t l, std::size_t& s) override { return in_->send(d, l, s); }
    hb::rtsp::IoStatus recv(std::uint8_t* b, std::size_t c, std::size_t& g) override { return in_->recv(b, c, g); }
    bool waitReadable(int t) override { return in_->waitReadable(t); }
    void close() override { in_->close(); }
    std::string lastError() const override { return in_->lastError(); }

private:
    std::unique_ptr<hb::rtsp::ITcpStream> in_;
};

}  // namespace hb::streaming::test
