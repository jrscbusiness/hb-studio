#pragma once
// Test-only helpers for the RTSP module: H.264/H.265 RTP packetizers, a scripted in-memory RTSP
// server and a fake ITcpStream network. No sockets, no sleeps.
#include <algorithm>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "rtsp/auth.hpp"
#include "rtsp/base64.hpp"
#include "rtsp/message.hpp"
#include "rtsp/rtp.hpp"
#include "rtsp/tcp_stream.hpp"
#include "rtsp/types.hpp"

namespace hb::rtsp::test {

inline TimePoint t0() { return TimePoint{} + std::chrono::seconds(1000); }

struct TestClock {
    TimePoint t = t0();
    TimePoint now() const { return t; }
    void advance(Millis d) { t += d; }
};

// ---------------------------------------------------------------- media generation
inline Bytes nalBytes(std::uint8_t header, std::size_t payload, std::uint8_t seed = 1) {
    Bytes b{header};
    for (std::size_t i = 0; i < payload; ++i) b.push_back(static_cast<std::uint8_t>(0x10 + ((i * 7 + seed) % 200)));
    return b;
}
inline Bytes sps264() { return {0x67, 0x42, 0xC0, 0x1F, 0xDA, 0x01, 0x40, 0x16, 0xE8}; }
inline Bytes pps264() { return {0x68, 0xCE, 0x3C, 0x80}; }
inline Bytes idr264(std::size_t n = 50, std::uint8_t seed = 1) { return nalBytes(0x65, n, seed); }
inline Bytes p264(std::size_t n = 20, std::uint8_t seed = 2) { return nalBytes(0x41, n, seed); }

inline Bytes vps265() { return {0x40, 0x01, 0x0C, 0x01, 0xFF, 0xFF}; }
inline Bytes sps265() { return {0x42, 0x01, 0x01, 0x01, 0x60, 0x00, 0x00}; }
inline Bytes pps265() { return {0x44, 0x01, 0xC1, 0x72, 0xB4}; }
inline Bytes idr265(std::size_t n = 50, std::uint8_t seed = 1) { Bytes b = nalBytes(0x26, n, seed); b.insert(b.begin() + 1, 0x01); return b; }
inline Bytes p265(std::size_t n = 20, std::uint8_t seed = 2) { Bytes b = nalBytes(0x02, n, seed); b.insert(b.begin() + 1, 0x01); return b; }

inline Bytes annexB(const std::vector<Bytes>& nals) {
    Bytes o;
    for (auto& n : nals) {
        o.insert(o.end(), {0, 0, 0, 1});
        o.insert(o.end(), n.begin(), n.end());
    }
    return o;
}

struct Packetizer {
    std::uint8_t pt = 96;
    std::uint32_t ssrc = 0x11223344;
    std::uint16_t seq = 1000;
    std::size_t mtu = 200;       // max RTP payload
    bool stap = true;            // aggregate small NALs (H.264 STAP-A / H.265 AP)

    Bytes emit(bool marker, std::uint32_t ts, const Bytes& payload) {
        return buildRtp(pt, marker, seq++, ts, ssrc, payload);
    }

    // returns complete RTP packets (header included), marker on the last one
    std::vector<Bytes> h264(const std::vector<Bytes>& nals, std::uint32_t ts) {
        std::vector<Bytes> payloads;
        std::size_t i = 0;
        while (i < nals.size()) {
            if (stap && nals[i].size() + 3 < mtu && i + 1 < nals.size() && nals[i + 1].size() + 3 < mtu &&
                (nals[i][0] & 0x1f) >= 6 && (nals[i][0] & 0x1f) <= 8) {
                Bytes agg{0x18};
                std::uint8_t nri = 0;
                std::size_t j = i;
                while (j < nals.size() && agg.size() + 2 + nals[j].size() <= mtu && (nals[j][0] & 0x1f) >= 6 &&
                       (nals[j][0] & 0x1f) <= 8) {
                    agg.push_back(static_cast<std::uint8_t>(nals[j].size() >> 8));
                    agg.push_back(static_cast<std::uint8_t>(nals[j].size()));
                    agg.insert(agg.end(), nals[j].begin(), nals[j].end());
                    nri = std::max<std::uint8_t>(nri, nals[j][0] & 0x60);
                    ++j;
                }
                agg[0] = static_cast<std::uint8_t>(0x18 | nri);
                payloads.push_back(agg);
                i = j;
                continue;
            }
            const Bytes& n = nals[i++];
            if (n.size() <= mtu) {
                payloads.push_back(n);
            } else {
                std::uint8_t ind = static_cast<std::uint8_t>((n[0] & 0xe0) | 28);
                std::size_t pos = 1, chunk = mtu - 2;
                while (pos < n.size()) {
                    std::size_t len = std::min(chunk, n.size() - pos);
                    std::uint8_t fh = n[0] & 0x1f;
                    if (pos == 1) fh |= 0x80;
                    if (pos + len == n.size()) fh |= 0x40;
                    Bytes p{ind, fh};
                    p.insert(p.end(), n.begin() + static_cast<std::ptrdiff_t>(pos), n.begin() + static_cast<std::ptrdiff_t>(pos + len));
                    payloads.push_back(std::move(p));
                    pos += len;
                }
            }
        }
        std::vector<Bytes> out;
        for (std::size_t k = 0; k < payloads.size(); ++k) out.push_back(emit(k + 1 == payloads.size(), ts, payloads[k]));
        return out;
    }

    std::vector<Bytes> h265(const std::vector<Bytes>& nals, std::uint32_t ts) {
        std::vector<Bytes> payloads;
        for (std::size_t i = 0; i < nals.size();) {
            auto type = [&](const Bytes& n) { return (n[0] >> 1) & 0x3f; };
            if (stap && i + 1 < nals.size() && type(nals[i]) >= 32 && type(nals[i]) <= 34) {
                Bytes agg{static_cast<std::uint8_t>(48 << 1), 0x01};
                std::size_t j = i;
                while (j < nals.size() && type(nals[j]) >= 32 && type(nals[j]) <= 34 &&
                       agg.size() + 2 + nals[j].size() <= mtu) {
                    agg.push_back(static_cast<std::uint8_t>(nals[j].size() >> 8));
                    agg.push_back(static_cast<std::uint8_t>(nals[j].size()));
                    agg.insert(agg.end(), nals[j].begin(), nals[j].end());
                    ++j;
                }
                payloads.push_back(agg);
                i = j;
                continue;
            }
            const Bytes& n = nals[i++];
            if (n.size() <= mtu) {
                payloads.push_back(n);
            } else {
                int t = type(n);
                std::size_t pos = 2, chunk = mtu - 3;
                while (pos < n.size()) {
                    std::size_t len = std::min(chunk, n.size() - pos);
                    std::uint8_t fh = static_cast<std::uint8_t>(t);
                    if (pos == 2) fh |= 0x80;
                    if (pos + len == n.size()) fh |= 0x40;
                    Bytes p{static_cast<std::uint8_t>((n[0] & 0x81) | (49 << 1)), n[1], fh};
                    p.insert(p.end(), n.begin() + static_cast<std::ptrdiff_t>(pos), n.begin() + static_cast<std::ptrdiff_t>(pos + len));
                    payloads.push_back(std::move(p));
                    pos += len;
                }
            }
        }
        std::vector<Bytes> out;
        for (std::size_t k = 0; k < payloads.size(); ++k) out.push_back(emit(k + 1 == payloads.size(), ts, payloads[k]));
        return out;
    }
};

// ---------------------------------------------------------------- scripted RTSP server
inline std::string defaultSdp264() {
    std::string sps = base64Encode(sps264()), pps = base64Encode(pps264());
    return "v=0\r\no=- 0 0 IN IP4 127.0.0.1\r\ns=Test Camera\r\nt=0 0\r\na=control:*\r\n"
           "m=video 0 RTP/AVP 96\r\nc=IN IP4 0.0.0.0\r\na=rtpmap:96 H264/90000\r\n"
           "a=fmtp:96 packetization-mode=1;profile-level-id=42C01F;sprop-parameter-sets=" + sps + "," + pps +
           "\r\na=control:trackID=1\r\na=framerate:25\r\na=x-dimensions:1920,1080\r\n"
           "m=audio 0 RTP/AVP 97\r\na=rtpmap:97 MPEG4-GENERIC/16000/1\r\na=control:trackID=2\r\n";
}

struct FakeServerConfig {
    enum class Auth { None, Basic, Digest } auth = Auth::None;
    std::string user = "admin", pass = "s3cretPass", realm = "HBTest";
    std::string sdp = defaultSdp264();
    std::string path = "/stream1";          // other paths -> 404
    bool supportGetParameter = true;
    std::string sessionId = "A1B2C3D4";
    int sessionTimeoutSec = 60;
    std::string redirectTo;                 // DESCRIBE -> 302 Location
    int setupStatus = 200;                  // e.g. 461
    bool staleOnce = false;                 // Digest: first authenticated request gets stale=true
    bool sessionExpired = false;            // all in-session requests -> 454
    bool replyTcpAsUdp = false;             // SETUP answers a UDP transport
    std::set<std::string> mute;             // methods that never get an answer
    std::set<std::string> authExempt = {};  // methods that do not need auth
    std::string contentBase;                // empty = request URI + "/"
    int optionsStatus = 200;                // e.g. 405 for cameras without OPTIONS
};

class FakeRtspServer {
public:
    explicit FakeRtspServer(FakeServerConfig cfg = {}) : cfg_(std::move(cfg)) { nonce_ = "nonce0001"; }

    // Client -> server bytes; returns what the server writes back.
    Bytes onBytes(ByteView in) {
        parser_.feed(in);
        Bytes out;
        MessageParser::Item item;
        while (parser_.next(item) == MessageParser::Status::Item) {
            if (auto* rq = std::get_if<Request>(&item)) {
                methods.push_back(rq->method);
                requests.push_back(*rq);
                append(out, handle(*rq));
            } else if (auto* f = std::get_if<InterleavedFrame>(&item)) {
                clientFrames.push_back(*f);
            }
        }
        return out;
    }

    static void append(Bytes& o, const std::string& s) { o.insert(o.end(), s.begin(), s.end()); }
    static void append(Bytes& o, const Bytes& s) { o.insert(o.end(), s.begin(), s.end()); }

    bool playing = false;
    int unauthorized = 0;
    int authorizedRequests = 0;
    std::vector<std::string> methods;
    std::vector<Request> requests;
    std::vector<InterleavedFrame> clientFrames;
    FakeServerConfig& config() { return cfg_; }

    int count(const std::string& m) const { return int(std::count(methods.begin(), methods.end(), m)); }

private:
    struct Params {
        std::map<std::string, std::string> kv;
    };
    static Params parseAuth(const std::string& v) {
        Params p;
        std::size_t i = v.find(' ');
        std::string rest = i == std::string::npos ? "" : v.substr(i + 1);
        std::size_t pos = 0;
        while (pos < rest.size()) {
            while (pos < rest.size() && (rest[pos] == ' ' || rest[pos] == ',')) ++pos;
            auto eq = rest.find('=', pos);
            if (eq == std::string::npos) break;
            std::string key = rest.substr(pos, eq - pos);
            pos = eq + 1;
            std::string val;
            if (pos < rest.size() && rest[pos] == '"') {
                auto e = rest.find('"', pos + 1);
                val = rest.substr(pos + 1, e - pos - 1);
                pos = e == std::string::npos ? rest.size() : e + 1;
            } else {
                auto e = rest.find(',', pos);
                val = rest.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
                pos = e == std::string::npos ? rest.size() : e;
            }
            p.kv[key] = val;
        }
        return p;
    }

    std::string response(const Request& rq, int status, const std::string& reason,
                         std::vector<std::pair<std::string, std::string>> extra = {}, const std::string& body = "") {
        Response r;
        r.status = status;
        r.reason = reason;
        r.headers.add("CSeq", rq.headers.get("CSeq").value_or("0"));
        for (auto& [k, v] : extra) r.headers.add(k, v);
        r.body = body;
        return serialize(r);
    }

    std::string challenge(const Request& rq, bool stale) {
        ++unauthorized;
        std::vector<std::pair<std::string, std::string>> h;
        if (cfg_.auth == FakeServerConfig::Auth::Basic) {
            h.push_back({"WWW-Authenticate", "Basic realm=\"" + cfg_.realm + "\""});
        } else {
            if (stale) nonce_ = "nonce" + std::to_string(++nonceCounter_);
            h.push_back({"WWW-Authenticate", "Digest realm=\"" + cfg_.realm + "\", nonce=\"" + nonce_ +
                                                 "\", qop=\"auth\"" + (stale ? ", stale=true" : "")});
        }
        return response(rq, 401, "Unauthorized", h);
    }

    bool authorized(const Request& rq) {
        auto a = rq.headers.get("Authorization");
        if (!a) return false;
        if (cfg_.auth == FakeServerConfig::Auth::Basic) {
            Credentials c{cfg_.user, cfg_.pass};
            return *a == basicHeaderValue(c);
        }
        Params p = parseAuth(*a);
        if (p.kv["username"] != cfg_.user || p.kv["nonce"] != nonce_ || p.kv["uri"] != rq.uri) return false;
        Challenge ch;
        ch.realm = cfg_.realm;
        ch.nonce = nonce_;
        std::string expect = digestResponse(ch, Credentials{cfg_.user, cfg_.pass}, rq.method, rq.uri, p.kv["qop"],
                                            p.kv["nc"], p.kv["cnonce"]);
        return expect == p.kv["response"];
    }

    Bytes handle(const Request& rq) {
        Bytes out;
        if (cfg_.mute.count(rq.method)) return out;
        if (cfg_.auth != FakeServerConfig::Auth::None && !cfg_.authExempt.count(rq.method)) {
            if (!authorized(rq)) {
                append(out, challenge(rq, false));
                return out;
            }
            if (cfg_.staleOnce) {
                cfg_.staleOnce = false;
                append(out, challenge(rq, true));
                return out;
            }
        }
        ++authorizedRequests;
        const std::string m = rq.method;
        std::string sess = cfg_.sessionId + ";timeout=" + std::to_string(cfg_.sessionTimeoutSec);
        std::string pub = "OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN";
        if (cfg_.supportGetParameter) pub += ", GET_PARAMETER";
        if (m == "OPTIONS") {
            if (cfg_.optionsStatus != 200) return toBytes(response(rq, cfg_.optionsStatus, "Method Not Allowed"));
            if (rq.headers.get("Session") && cfg_.sessionExpired) return reply454(rq);
            append(out, response(rq, 200, "OK", {{"Public", pub}}));
        } else if (m == "DESCRIBE") {
            if (!cfg_.redirectTo.empty()) {
                append(out, response(rq, 302, "Found", {{"Location", cfg_.redirectTo}}));
            } else if (rq.uri.find(cfg_.path) == std::string::npos) {
                append(out, response(rq, 404, "Not Found"));
            } else {
                std::string base = cfg_.contentBase.empty() ? rq.uri + "/" : cfg_.contentBase;
                append(out, response(rq, 200, "OK", {{"Content-Base", base}, {"Content-Type", "application/sdp"}}, cfg_.sdp));
            }
        } else if (m == "SETUP") {
            if (cfg_.setupStatus != 200) {
                append(out, response(rq, cfg_.setupStatus, "Error"));
            } else {
                std::string tr = rq.headers.get("Transport").value_or("");
                std::string reply = cfg_.replyTcpAsUdp ? "RTP/AVP;unicast;client_port=50000-50001;server_port=6970-6971"
                                                       : "RTP/AVP/TCP;unicast;interleaved=0-1;ssrc=11223344";
                lastSetupUri = rq.uri;
                lastTransport = tr;
                append(out, response(rq, 200, "OK", {{"Session", sess}, {"Transport", reply}}));
            }
        } else if (m == "PLAY") {
            if (cfg_.sessionExpired) return reply454(rq);
            playing = true;
            append(out, response(rq, 200, "OK", {{"Session", cfg_.sessionId}, {"RTP-Info", "url=" + lastSetupUri + ";seq=1000;rtptime=0"}}));
        } else if (m == "GET_PARAMETER") {
            if (cfg_.sessionExpired) return reply454(rq);
            append(out, response(rq, 200, "OK", {{"Session", cfg_.sessionId}}));
        } else if (m == "TEARDOWN") {
            playing = false;
            append(out, response(rq, 200, "OK", {{"Session", cfg_.sessionId}}));
        } else {
            append(out, response(rq, 501, "Not Implemented"));
        }
        return out;
    }

    Bytes reply454(const Request& rq) {
        Bytes o;
        append(o, response(rq, 454, "Session Not Found"));
        return o;
    }

    FakeServerConfig cfg_;
    MessageParser parser_;
    std::string nonce_;
    int nonceCounter_ = 1;

public:
    std::string lastSetupUri, lastTransport;
};

// ---------------------------------------------------------------- fake network
struct FakeConnection {
    explicit FakeConnection(FakeServerConfig c) : server(std::move(c)) {}
    FakeRtspServer server;
    std::deque<std::uint8_t> toClient;
    bool serverClosed = false;
    bool blackhole = false;       // swallow everything in both directions (dead link)
    std::size_t recvChunk = 65536;  // max bytes per recv (fragmentation stress)
    bool clientClosed = false;
    std::size_t bytesFromClient = 0;
    std::string target;           // host:port the client asked for
    int index = 0;

    void serverSend(const Bytes& b) {
        if (blackhole) return;
        toClient.insert(toClient.end(), b.begin(), b.end());
    }
    void serverSendInterleaved(std::uint8_t ch, const Bytes& rtp) { serverSend(frameInterleaved(ch, rtp)); }
    void closeFromServer() { serverClosed = true; }
};

class FakeTcpStream final : public ITcpStream {
public:
    FakeTcpStream(std::shared_ptr<FakeConnection> c, IoStatus connectResult, int pendingPolls, std::string err)
        : conn_(std::move(c)), result_(connectResult), pending_(pendingPolls), err_(std::move(err)) {}
    IoStatus connect(const std::string& host, std::uint16_t port) override {
        conn_->target = host + ":" + std::to_string(port);
        if (result_ == IoStatus::Error) return IoStatus::Error;
        if (pending_ > 0 || pending_ < 0) return IoStatus::WouldBlock;
        connected_ = true;
        return IoStatus::Ok;
    }
    IoStatus pollConnect() override {
        if (connected_) return IoStatus::Ok;
        if (pending_ < 0) return IoStatus::WouldBlock;  // hangs forever
        if (--pending_ <= 0) { connected_ = true; return IoStatus::Ok; }
        return IoStatus::WouldBlock;
    }
    IoStatus send(const std::uint8_t* d, std::size_t n, std::size_t& sent) override {
        sent = 0;
        if (conn_->serverClosed) { err_ = "broken pipe"; return IoStatus::Error; }
        conn_->bytesFromClient += n;
        sent = n;
        if (conn_->blackhole) return IoStatus::Ok;
        conn_->serverSend(conn_->server.onBytes(ByteView(d, n)));
        return IoStatus::Ok;
    }
    IoStatus recv(std::uint8_t* buf, std::size_t cap, std::size_t& got) override {
        got = 0;
        if (!conn_->toClient.empty()) {
            std::size_t n = std::min({cap, conn_->toClient.size(), conn_->recvChunk});
            for (std::size_t i = 0; i < n; ++i) { buf[i] = conn_->toClient.front(); conn_->toClient.pop_front(); }
            got = n;
            return IoStatus::Ok;
        }
        if (conn_->serverClosed) return IoStatus::Closed;
        return IoStatus::WouldBlock;
    }
    bool waitReadable(int) override { return !conn_->toClient.empty(); }
    void close() override { conn_->clientClosed = true; }
    std::string lastError() const override { return err_; }

private:
    std::shared_ptr<FakeConnection> conn_;
    IoStatus result_;
    int pending_;
    bool connected_ = false;
    std::string err_;
};

class FakeNetwork {
public:
    FakeServerConfig serverCfg;
    int refuseNext = 0;            // next N connect() calls fail
    int connectPolls = 0;          // pollConnect() needs this many polls (-1 = never completes)
    std::size_t recvChunk = 65536;
    std::vector<std::shared_ptr<FakeConnection>> connections;
    std::function<void(FakeServerConfig&, int index)> customize;  // tweak the server of connection #index

    TcpStreamFactory factory() {
        return [this]() -> std::unique_ptr<ITcpStream> {
            FakeServerConfig fc = serverCfg;
            int idx = static_cast<int>(connections.size());
            if (customize) customize(fc, idx);
            auto c = std::make_shared<FakeConnection>(fc);
            c->index = idx;
            c->recvChunk = recvChunk;
            connections.push_back(c);
            if (refuseNext > 0) {
                --refuseNext;
                return std::make_unique<FakeTcpStream>(c, IoStatus::Error, 0, "connection refused");
            }
            return std::make_unique<FakeTcpStream>(c, IoStatus::Ok, connectPolls, "");
        };
    }
    std::shared_ptr<FakeConnection> last() { return connections.empty() ? nullptr : connections.back(); }
};

}  // namespace hb::rtsp::test
