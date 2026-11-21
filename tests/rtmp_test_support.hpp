#pragma once
// A minimal scripted RTMP server used only by rtmp_client_test.cpp's real-socket loopback tests -
// it understands exactly the subset of RTMP a well-behaved publish client (this project's
// RtmpClient) sends: the simple handshake, chunk streams using fmt 0/1/2/3 with 1-byte basic
// headers, and the connect/createStream/publish command sequence. It is deliberately not a general
// RTMP server implementation.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "rtsp/tcp_stream.hpp"
#include "streaming/amf0.hpp"

namespace hb::streaming::test {

inline void put24be(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>(v & 0xFF));
}
inline void put32be(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>(v & 0xFF));
}
inline void put32le(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFF));
}
inline std::uint32_t get24be(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 16) | (static_cast<std::uint32_t>(p[1]) << 8) | p[2];
}

inline bool recvExact(hb::rtsp::ITcpStream& s, std::uint8_t* buf, std::size_t len, std::atomic<bool>& stop, int maxMs) {
    std::size_t got = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(maxMs);
    while (got < len) {
        if (stop || std::chrono::steady_clock::now() >= deadline) return false;
        std::size_t n = 0;
        const auto st = s.recv(buf + got, len - got, n);
        if (st == hb::rtsp::IoStatus::Ok) got += n;
        else if (st == hb::rtsp::IoStatus::WouldBlock) s.waitReadable(20);
        else return false;
    }
    return true;
}
inline bool sendAll(hb::rtsp::ITcpStream& s, const std::vector<std::uint8_t>& b, std::atomic<bool>& stop) {
    std::size_t off = 0;
    while (off < b.size()) {
        if (stop) return false;
        std::size_t n = 0;
        const auto st = s.send(b.data() + off, b.size() - off, n);
        if (st == hb::rtsp::IoStatus::Ok) off += n;
        else if (st == hb::rtsp::IoStatus::WouldBlock) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        else return false;
    }
    return true;
}

struct FakeRtmpServerResult {
    bool connected = false, published = false;
    int videoMessages = 0, audioMessages = 0;
    std::string lastStreamKey;
};

// Serves exactly one client to completion of the publish handshake, then keeps counting media
// messages until `stop` is set or `maxMs` elapses.
inline FakeRtmpServerResult serveOneRtmpClient(hb::rtsp::ITcpStream& s, std::atomic<bool>& stop, int maxMs) {
    FakeRtmpServerResult r;
    // ---- handshake ----
    std::uint8_t c0;
    if (!recvExact(s, &c0, 1, stop, maxMs)) return r;
    std::vector<std::uint8_t> c1(1536);
    if (!recvExact(s, c1.data(), c1.size(), stop, maxMs)) return r;
    std::vector<std::uint8_t> s1(1536, 0);
    std::vector<std::uint8_t> s0s1s2;
    s0s1s2.push_back(0x03);
    s0s1s2.insert(s0s1s2.end(), s1.begin(), s1.end());
    s0s1s2.insert(s0s1s2.end(), c1.begin(), c1.end());   // S2 = echo of C1, same convention as the client's C2=echo(S1)
    if (!sendAll(s, s0s1s2, stop)) return r;
    std::vector<std::uint8_t> c2(1536);
    if (!recvExact(s, c2.data(), c2.size(), stop, maxMs)) return r;

    // ---- chunk stream de-multiplexing (mirrors RtmpClient::readMessage, server side) ----
    struct Csid { std::uint32_t ts = 0, length = 0, streamId = 0; std::uint8_t typeId = 0; std::vector<std::uint8_t> payload; std::size_t have = 0; bool extTs = false; };
    std::map<std::uint32_t, Csid> states;
    std::uint32_t peerChunkSize = 128;
    std::uint32_t createdStreamId = 1;
    double connectTxn = 0, createStreamTxn = 0;

    auto sendMessage = [&](std::uint8_t csid, std::uint8_t typeId, std::uint32_t streamId, const std::vector<std::uint8_t>& payload) {
        std::vector<std::uint8_t> h;
        h.push_back(csid & 0x3F);
        put24be(h, 0);
        put24be(h, static_cast<std::uint32_t>(payload.size()));
        h.push_back(typeId);
        put32le(h, streamId);
        h.insert(h.end(), payload.begin(), payload.end());
        return sendAll(s, h, stop);
    };
    auto sendCommand = [&](const std::string& name, double txn, std::vector<amf0::Value> args) {
        std::vector<std::uint8_t> body;
        amf0::encode(amf0::Value::string(name), body);
        amf0::encode(amf0::Value::number(txn), body);
        for (auto& a : args) amf0::encode(a, body);
        return sendMessage(3, 20, 0, body);
    };

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(maxMs);
    while (!stop && std::chrono::steady_clock::now() < deadline) {
        std::uint8_t b0;
        if (!recvExact(s, &b0, 1, stop, maxMs)) break;
        const std::uint8_t fmt = static_cast<std::uint8_t>(b0 >> 6);
        std::uint32_t csid = b0 & 0x3F;   // the client under test never sends csid>=64
        auto& st = states[csid];
        if (fmt <= 2) {
            std::uint8_t ts3[3];
            if (!recvExact(s, ts3, 3, stop, maxMs)) break;
            std::uint32_t ts = get24be(ts3);
            if (fmt <= 1) {
                std::uint8_t rest[4];
                if (!recvExact(s, rest, 4, stop, maxMs)) break;
                st.length = get24be(rest);
                st.typeId = rest[3];
                if (fmt == 0) {
                    std::uint8_t sid[4];
                    if (!recvExact(s, sid, 4, stop, maxMs)) break;
                    st.streamId = static_cast<std::uint32_t>(sid[0]) | (static_cast<std::uint32_t>(sid[1]) << 8) |
                                  (static_cast<std::uint32_t>(sid[2]) << 16) | (static_cast<std::uint32_t>(sid[3]) << 24);
                }
            }
            st.extTs = ts == 0xFFFFFF;
            if (st.extTs) { std::uint8_t e[4]; if (!recvExact(s, e, 4, stop, maxMs)) break; }
            if (st.have == 0) st.payload.assign(st.length, 0);
        } else {
            if (st.extTs) { std::uint8_t e[4]; if (!recvExact(s, e, 4, stop, maxMs)) break; }
            if (st.have == 0) st.payload.assign(st.length, 0);
        }
        const std::size_t want = std::min<std::size_t>(peerChunkSize, st.length - st.have);
        if (want && !recvExact(s, st.payload.data() + st.have, want, stop, maxMs)) break;
        st.have += want;
        if (st.have < st.length) continue;
        st.have = 0;

        if (st.typeId == 1 && st.payload.size() >= 4) {   // Set Chunk Size
            peerChunkSize = (static_cast<std::uint32_t>(st.payload[0]) << 24) | get24be(st.payload.data() + 1);
            continue;
        }
        if (st.typeId == 9) { ++r.videoMessages; continue; }
        if (st.typeId == 8) { ++r.audioMessages; continue; }
        if (st.typeId != 20) continue;   // not an AMF0 command

        auto vals = amf0::decodeAll(st.payload.data(), st.payload.size());
        if (vals.empty() || vals[0].type != amf0::Value::Type::String) continue;
        const std::string& cmd = vals[0].str;
        if (cmd == "connect") {
            connectTxn = vals.size() > 1 ? vals[1].num : 1;
            r.connected = true;
            sendCommand("_result", connectTxn, {amf0::Value::object({{"fmsVer", amf0::Value::string("HBFake/1,0")}}),
                                                amf0::Value::object({{"level", amf0::Value::string("status")},
                                                                      {"code", amf0::Value::string("NetConnection.Connect.Success")}})});
        } else if (cmd == "createStream") {
            createStreamTxn = vals.size() > 1 ? vals[1].num : 2;
            sendCommand("_result", createStreamTxn, {amf0::Value::null(), amf0::Value::number(createdStreamId)});
        } else if (cmd == "publish") {
            if (vals.size() > 3 && vals[3].type == amf0::Value::Type::String) r.lastStreamKey = vals[3].str;
            r.published = true;
            sendCommand("onStatus", 0, {amf0::Value::null(), amf0::Value::object({{"level", amf0::Value::string("status")},
                                                                                   {"code", amf0::Value::string("NetStream.Publish.Start")}})});
        }
        (void)connectTxn;
    }
    return r;
}

}  // namespace hb::streaming::test
