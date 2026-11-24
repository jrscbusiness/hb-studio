#include "remote/server.hpp"

#include <algorithm>
#include <map>

#include "core/json.hpp"
#include "remote/http.hpp"
#include "remote/lan.hpp"
#include "remote/websocket.hpp"

namespace hb::remote {

using hb::rtsp::IoStatus;
using hb::rtsp::ITcpStream;

struct RemoteServer::Client {
    std::unique_ptr<ITcpStream> s;
    std::string ip;
    enum class Phase { Http, Ws } phase = Phase::Http;
    http::RequestParser httpParser;
    ws::Parser parser;
    bool authed = false;
    std::uint64_t session = 0;
    int pairAttempts = 0;
    bool thumbsOn = false;
    std::deque<std::vector<std::uint8_t>> ctrl;
    std::vector<std::uint8_t> out;
    std::size_t outPos = 0;
    Clock::time_point connected, lastRecv, lastPing, lastProgress, lastStateSent, lastSessionCheck, lastRefill, thumbRefill, closeDeadline;
    std::uint64_t stateSeqSent = 0;
    std::uint64_t thumbSeqSent[2] = {0, 0};
    int thumbToggle = 0;
    double thumbTokens = 0;
    double cmdTokens = 0;
    bool closing = false;  // flush ctrl frames then drop; no more data frames

    Client(std::unique_ptr<ITcpStream> st, std::string peer, const ServerConfig& cfg)
        : s(std::move(st)), ip(std::move(peer)), httpParser(cfg.maxHttpHeadBytes), parser(ws::ParserLimits{cfg.maxInboundMessageBytes}, true) {}
};

namespace {
std::string jstr(const std::string& s) { return "\"" + core::json::escape(s) + "\""; }
}  // namespace

RemoteServer::RemoteServer(ServerConfig cfg, AuthManager& auth, CommandSink sink, LogSink log)
    : cfg_(std::move(cfg)), auth_(auth), sink_(std::move(sink)), log_(std::move(log)) {}

RemoteServer::~RemoteServer() { stop(); }

bool RemoteServer::start(std::string& error) {
    if (running_) return true;
    listeners_.clear();
    bound_.clear();
    port_ = 0;
    std::uint16_t port = cfg_.port;
    std::string firstErr;
    for (const auto& addr : cfg_.bindAddresses) {
        auto l = hb::rtsp::makeTcpListener(addr, port);
        if (!l) {
            if (firstErr.empty()) firstErr = "não foi possível abrir " + addr + ":" + std::to_string(port);
            continue;
        }
        if (port == 0) port = l->port();  // later interfaces share the port the first one got
        listeners_.push_back(std::move(l));
        bound_.push_back(addr);
    }
    if (listeners_.empty()) {
        error = firstErr.empty() ? "nenhuma interface de rede disponível" : firstErr;
        return false;
    }
    port_ = listeners_.front()->port();
    {
        std::lock_guard lock(execMu_);
        execQuit_ = false;
        execQ_.clear();
    }
    running_ = true;
    exec_ = std::thread([this] { execLoop(); });
    io_ = std::thread([this] { ioLoop(); });
    return true;
}

void RemoteServer::stop() {
    if (!running_.exchange(false)) {
        if (io_.joinable()) io_.join();
        if (exec_.joinable()) exec_.join();
        return;
    }
    if (io_.joinable()) io_.join();
    {
        std::lock_guard lock(execMu_);
        execQuit_ = true;
    }
    execCv_.notify_all();
    if (exec_.joinable()) exec_.join();
    listeners_.clear();
    bound_.clear();
    {
        std::lock_guard lock(liveMu_);
        live_.clear();
    }
    thumbClients_ = 0;
    clientCount_ = 0;
}

void RemoteServer::broadcastState(const std::string& json) {
    std::lock_guard lock(mailMu_);
    if (json == latestState_) return;
    latestState_ = json;
    ++stateSeq_;
}

void RemoteServer::broadcastThumb(int kind, const std::vector<std::uint8_t>& jpeg) {
    if (kind < 0 || kind > 1 || jpeg.empty() || jpeg.size() > cfg_.maxThumbBytes || !wantsThumbs()) return;
    std::lock_guard lock(mailMu_);
    latestThumb_[kind].assign(jpeg.begin(), jpeg.end());
    ++thumbSeq_[kind];
}

std::vector<RemoteServer::ConnectedDevice> RemoteServer::connectedDevices() const {
    std::lock_guard lock(liveMu_);
    return live_;
}

void RemoteServer::execLoop() {
    for (;;) {
        std::string cmd;
        {
            std::unique_lock lock(execMu_);
            execCv_.wait(lock, [this] { return execQuit_ || !execQ_.empty(); });
            if (execQ_.empty()) return;  // quit and drained
            cmd = std::move(execQ_.front());
            execQ_.pop_front();
        }
        try {
            if (sink_) sink_(cmd);
        } catch (...) {
            logLine("remote: command handler threw");
        }
    }
}

void RemoteServer::ioLoop() {
    std::vector<std::unique_ptr<Client>> clients;
    while (running_) {
        const auto now = Clock::now();
        bool activity = false;
        // Accept.
        for (auto& l : listeners_) {
            for (int k = 0; k < 4; ++k) {
                auto st = l->accept(0);
                if (!st) break;
                activity = true;
                std::string peer = st->peerAddress();
                std::size_t unauth = 0, sameIp = 0;
                for (const auto& c : clients) {
                    if (!c->authed) ++unauth;
                    if (c->ip == peer) ++sameIp;
                }
                if (cfg_.lanPeersOnly && !isPrivateIpv4(peer)) {
                    logLine("remote: conexão recusada (origem fora da rede local)");
                    continue;  // dropped silently: destructor closes the socket
                }
                if (clients.size() >= cfg_.maxConnections || unauth >= cfg_.maxUnauthenticated || sameIp >= cfg_.maxPerIp) {
                    const std::string r = http::serialize({503, "text/plain; charset=utf-8", "busy", {}});
                    std::size_t sent = 0;
                    st->send(reinterpret_cast<const std::uint8_t*>(r.data()), r.size(), sent);  // best effort
                    ++dropped_;
                    continue;
                }
                auto c = std::make_unique<Client>(std::move(st), std::move(peer), cfg_);
                c->connected = c->lastRecv = c->lastPing = c->lastProgress = c->lastSessionCheck = c->lastRefill = c->thumbRefill = now;
                c->cmdTokens = cfg_.commandsPerSec;
                clients.push_back(std::move(c));
            }
        }
        // Service clients.
        for (auto it = clients.begin(); it != clients.end();) {
            bool keep = false;
            try {
                keep = service(**it, now, activity);
            } catch (...) {
                keep = false;
            }
            if (!keep) it = clients.erase(it);
            else ++it;
        }
        publishLive(clients);
        clientCount_ = clients.size();
        if (!activity) std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
    // Shutdown: best-effort close frame, then sockets close with the Client destructors.
    for (auto& c : clients) {
        if (c->phase == Client::Phase::Ws) {
            const auto f = ws::encodeClose(1001, "server stopping");
            std::size_t sent = 0;
            c->s->send(f.data(), f.size(), sent);
        }
    }
}

void RemoteServer::publishLive(const std::vector<std::unique_ptr<Client>>& clients) {
    std::vector<ConnectedDevice> v;
    int thumbs = 0;
    for (const auto& c : clients) {
        if (!c->authed) continue;
        v.push_back({c->session, c->thumbsOn});
        if (c->thumbsOn) ++thumbs;
    }
    thumbClients_ = thumbs;
    std::lock_guard lock(liveMu_);
    live_ = std::move(v);
}

bool RemoteServer::service(Client& c, Clock::time_point now, bool& activity) {
    if (!pumpRead(c, now, activity)) return false;
    // Timers.
    const auto ms = [&](Clock::time_point t) { return std::chrono::duration_cast<std::chrono::milliseconds>(now - t).count(); };
    if (c.phase == Client::Phase::Http && ms(c.connected) > cfg_.httpTimeoutMs) return false;
    if (c.phase == Client::Phase::Ws) {
        if (!c.authed && !c.closing && ms(c.connected) > cfg_.authTimeoutMs) {
            queueClose(c, 1008, "auth timeout", now);
            ++dropped_;
        }
        if (ms(c.lastRecv) > cfg_.idleTimeoutMs) return false;
        if (!c.closing && ms(c.lastPing) > cfg_.pingIntervalMs) {
            c.lastPing = now;
            c.ctrl.push_back(ws::encodeFrame(ws::Opcode::Ping, nullptr, 0));
        }
        if (c.authed && !c.closing && ms(c.lastSessionCheck) >= 250) {
            c.lastSessionCheck = now;
            if (!auth_.valid(c.session, now)) queueClose(c, 1008, "session ended", now);
        }
    }
    if (c.ctrl.size() > cfg_.maxCtrlQueue) {
        ++dropped_;
        return false;
    }
    return pumpWrite(c, now, activity);
}

bool RemoteServer::pumpRead(Client& c, Clock::time_point now, bool& activity) {
    std::uint8_t buf[4096];
    for (int round = 0; round < 4; ++round) {
        std::size_t got = 0;
        const IoStatus st = c.s->recv(buf, sizeof buf, got);
        if (st == IoStatus::WouldBlock) return true;
        if (st != IoStatus::Ok) return false;  // Closed / Error
        activity = true;
        c.lastRecv = now;
        if (c.closing) continue;  // discard whatever arrives while we are closing
        if (c.phase == Client::Phase::Http) {
            const auto r = c.httpParser.feed(buf, got);
            if (r == http::RequestParser::State::Error) {
                respondAndClose(c, c.httpParser.errorStatus(), "bad request", now);
            } else if (r == http::RequestParser::State::Done) {
                if (!handleHttp(c, now)) return false;
            }
        } else {
            std::vector<ws::Message> msgs;
            const auto pr = c.parser.feed(buf, got, msgs);
            for (auto& m : msgs) {
                if (c.closing) break;
                switch (m.type) {
                    case ws::Opcode::Ping:
                        c.ctrl.push_back(ws::encodeFrame(ws::Opcode::Pong, reinterpret_cast<const std::uint8_t*>(m.data.data()), m.data.size()));
                        break;
                    case ws::Opcode::Pong:
                        break;
                    case ws::Opcode::Close:
                        queueClose(c, m.closeCode == 1005 ? 1000 : 1000, "", now);
                        break;
                    case ws::Opcode::Binary:
                        queueClose(c, 1003, "binary not supported", now);
                        break;
                    case ws::Opcode::Text:
                        if (!handleWsMessage(c, m.data, now)) queueClose(c, 1008, "policy", now);
                        break;
                    default:
                        break;
                }
            }
            if (!pr.ok && !c.closing) {
                queueClose(c, pr.closeCode ? pr.closeCode : 1002, "protocol error", now);
                ++dropped_;
            }
        }
    }
    return true;
}

void RemoteServer::respondAndClose(Client& c, int status, const std::string& body, Clock::time_point now) {
    const std::string r = http::serialize({status, "text/plain; charset=utf-8", body, {}});
    c.out.assign(r.begin(), r.end());
    c.outPos = 0;
    c.lastProgress = now;
    c.closing = true;
    c.closeDeadline = now + std::chrono::milliseconds(1500);
}

bool RemoteServer::handleHttp(Client& c, Clock::time_point now) {
    const http::Request& req = c.httpParser.request();
    const std::string* host = req.header("host");
    // DNS-rebinding defence: only IP-literal / localhost Host headers are served.
    if (!host || !http::hostIsIpLiteralOrLocalhost(*host)) {
        respondAndClose(c, 403, "forbidden host", now);
        return true;
    }
    if (req.method != "GET") {
        respondAndClose(c, 405, "method not allowed", now);
        return true;
    }
    if (req.path == "/") {
        http::Response r;
        r.contentType = "text/html; charset=utf-8";
        r.body = cfg_.pageHtml;
        const std::string s = http::serialize(r);
        c.out.assign(s.begin(), s.end());
        c.outPos = 0;
        c.lastProgress = now;
        c.closing = true;
        c.closeDeadline = now + std::chrono::milliseconds(cfg_.stallTimeoutMs);
        return true;
    }
    if (req.path != "/ws") {
        respondAndClose(c, 404, "not found", now);
        return true;
    }
    // ---- WebSocket upgrade ----
    const std::string* up = req.header("upgrade");
    const std::string* conn = req.header("connection");
    const std::string* ver = req.header("sec-websocket-version");
    const std::string* key = req.header("sec-websocket-key");
    const std::string* origin = req.header("origin");
    if (!up || !conn || !http::headerHasToken(*up, "websocket") || !http::headerHasToken(*conn, "upgrade")) {
        respondAndClose(c, 426, "upgrade required", now);
        return true;
    }
    // Cross-site WebSocket hijacking defence: browsers always send Origin; it must be this very server.
    if (!origin || !http::originMatchesHost(*origin, *host)) {
        respondAndClose(c, 403, "bad origin", now);
        return true;
    }
    if (!ver || *ver != "13" || !key || !ws::validClientKey(*key)) {
        respondAndClose(c, 400, "bad websocket handshake", now);
        return true;
    }
    if (!c.httpParser.leftover().empty()) {
        respondAndClose(c, 400, "unexpected data", now);
        return true;
    }
    const std::string resp = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " +
                             ws::acceptKey(*key) + "\r\nCache-Control: no-store\r\n\r\n";
    c.out.assign(resp.begin(), resp.end());
    c.outPos = 0;
    c.lastProgress = now;
    c.phase = Client::Phase::Ws;
    c.connected = c.lastRecv = c.lastPing = now;  // auth timer starts at upgrade
    return true;
}

void RemoteServer::queueText(Client& c, const std::string& json) { c.ctrl.push_back(ws::encodeText(json)); }

void RemoteServer::queueClose(Client& c, std::uint16_t code, const char* reason, Clock::time_point now) {
    if (c.closing) return;
    c.closing = true;
    c.closeDeadline = now + std::chrono::milliseconds(1500);
    c.ctrl.push_back(ws::encodeClose(code, reason));
}

bool RemoteServer::handleWsMessage(Client& c, const std::string& text, Clock::time_point now) {
    core::json::Value v;
    core::json::Limits lim;
    lim.maxBytes = cfg_.maxInboundMessageBytes;
    lim.maxDepth = 4;
    lim.maxArray = 16;
    if (!core::json::parse(text, v, lim) || !v.isObj()) return false;
    const core::json::Value* t = v.get("t");
    if (!t || !t->isStr()) return false;
    const std::string& type = t->s;

    auto hello = [&] {
        queueText(c, "{\"t\":\"hello\",\"v\":1,\"name\":" + jstr(std::string()) + "}");
        c.stateSeqSent = 0;  // force an immediate state push
        c.lastStateSent = now - std::chrono::seconds(1);
    };

    if (!c.authed) {
        if (type == "auth") {
            const core::json::Value* tok = v.get("token");
            if (!tok || !tok->isStr() || tok->s.size() > 128) return false;
            if (const auto id = auth_.authenticate(tok->s, now)) {
                c.authed = true;
                c.session = *id;
                hello();
            } else {
                queueText(c, "{\"t\":\"authfail\"}");
                queueClose(c, 1008, "auth failed", now);
            }
            return true;
        }
        if (type == "pair") {
            const core::json::Value* pin = v.get("pin");
            const core::json::Value* name = v.get("name");
            if (!pin || !pin->isStr() || pin->s.size() > 16) return false;
            const std::string nm = name && name->isStr() ? name->s.substr(0, 200) : std::string();
            const PairOutcome o = auth_.pair(pin->s, nm, c.ip, now);
            switch (o.result) {
                case PairResult::Ok:
                    c.authed = true;
                    c.session = o.sessionId;
                    queueText(c, "{\"t\":\"paired\",\"token\":" + jstr(o.token) + "}");
                    logLine("remote: dispositivo pareado");
                    hello();
                    break;
                case PairResult::BadPin:
                    queueText(c, "{\"t\":\"pairfail\",\"r\":\"bad_pin\",\"left\":" + std::to_string(o.attemptsLeft) + "}");
                    if (++c.pairAttempts >= 3) queueClose(c, 1008, "too many attempts", now);
                    break;
                case PairResult::Locked:
                    queueText(c, "{\"t\":\"pairfail\",\"r\":\"locked\",\"ms\":" + std::to_string(o.retryAfter.count()) + "}");
                    queueClose(c, 1008, "locked", now);
                    break;
                case PairResult::TooManySessions:
                    queueText(c, "{\"t\":\"pairfail\",\"r\":\"full\"}");
                    queueClose(c, 1008, "full", now);
                    break;
                case PairResult::RandomFailure:
                    queueText(c, "{\"t\":\"pairfail\",\"r\":\"error\"}");
                    queueClose(c, 1011, "error", now);
                    break;
            }
            return true;
        }
        return false;  // anything else before authentication is a policy violation
    }

    auth_.touch(c.session, now);
    if (type == "p") {
        queueText(c, "{\"t\":\"pong\"}");
        return true;
    }
    if (type == "thumbs") {
        const core::json::Value* on = v.get("on");
        if (!on || !on->isBool()) return false;
        c.thumbsOn = on->b;
        c.thumbSeqSent[0] = c.thumbSeqSent[1] = 0;
        // Fresh bucket: first thumbnails go out right away.
        c.thumbTokens = cfg_.thumbBytesPerSec;
        c.thumbRefill = now;
        return true;
    }
    if (type == "cmd") {
        const double dt = std::chrono::duration<double>(now - c.lastRefill).count();
        c.lastRefill = now;
        c.cmdTokens = std::min<double>(cfg_.commandsPerSec, c.cmdTokens + dt * cfg_.commandsPerSec);
        if (c.cmdTokens < 1.0) {
            queueText(c, "{\"t\":\"err\",\"e\":\"rate_limited\"}");
            return true;
        }
        c.cmdTokens -= 1.0;
        const Translation tr = translate(v, cfg_.commandLimits);
        if (!tr.ok) {
            queueText(c, "{\"t\":\"err\",\"e\":" + jstr(tr.error) + "}");
            return true;
        }
        bool queued = false;
        {
            std::lock_guard lock(execMu_);
            if (execQ_.size() < 256) {
                execQ_.push_back(tr.hostCommand);
                queued = true;
            }
        }
        if (queued) execCv_.notify_one();
        else queueText(c, "{\"t\":\"err\",\"e\":\"busy\"}");
        return true;
    }
    return false;
}

bool RemoteServer::fillNext(Client& c, Clock::time_point now) {
    if (!c.ctrl.empty()) {
        c.out = std::move(c.ctrl.front());
        c.ctrl.pop_front();
        c.outPos = 0;
        c.lastProgress = now;
        return true;
    }
    if (c.closing || c.phase != Client::Phase::Ws || !c.authed) return false;
    const auto sinceState = std::chrono::duration_cast<std::chrono::milliseconds>(now - c.lastStateSent).count();
    if (sinceState >= cfg_.stateMinIntervalMs) {
        std::string json;
        {
            std::lock_guard lock(mailMu_);
            if (stateSeq_ != c.stateSeqSent) {
                json = latestState_;
                c.stateSeqSent = stateSeq_;
            }
        }
        if (!json.empty()) {
            c.out = ws::encodeText(json);
            c.outPos = 0;
            c.lastStateSent = now;
            c.lastProgress = now;
            return true;
        }
    }
    if (c.thumbsOn) {
        const double dt = std::chrono::duration<double>(now - c.thumbRefill).count();
        c.thumbTokens = std::min<double>(cfg_.thumbBytesPerSec, c.thumbTokens + dt * cfg_.thumbBytesPerSec);
        c.thumbRefill = now;
        if (c.thumbTokens > 0) {
            for (int n = 0; n < 2; ++n) {
                const int kind = (c.thumbToggle + n) & 1;
                std::vector<std::uint8_t> frame;
                {
                    std::lock_guard lock(mailMu_);
                    if (thumbSeq_[kind] != c.thumbSeqSent[kind] && !latestThumb_[kind].empty()) {
                        frame.reserve(latestThumb_[kind].size() + 1);
                        frame.push_back(static_cast<std::uint8_t>(kind));
                        frame.insert(frame.end(), latestThumb_[kind].begin(), latestThumb_[kind].end());
                        c.thumbSeqSent[kind] = thumbSeq_[kind];
                    }
                }
                if (!frame.empty()) {
                    c.thumbTokens -= static_cast<double>(frame.size());
                    c.thumbToggle = kind ^ 1;
                    c.out = ws::encodeFrame(ws::Opcode::Binary, frame.data(), frame.size());
                    c.outPos = 0;
                    c.lastProgress = now;
                    return true;
                }
            }
        }
    }
    return false;
}

bool RemoteServer::pumpWrite(Client& c, Clock::time_point now, bool& activity) {
    for (int round = 0; round < 8; ++round) {
        if (c.outPos >= c.out.size()) {
            c.out.clear();
            c.outPos = 0;
            if (!fillNext(c, now)) break;
        }
        std::size_t sent = 0;
        const IoStatus st = c.s->send(c.out.data() + c.outPos, c.out.size() - c.outPos, sent);
        if (st == IoStatus::Ok) {
            c.outPos += sent;
            c.lastProgress = now;
            activity = true;
        } else if (st == IoStatus::WouldBlock) {
            break;
        } else {
            return false;
        }
    }
    const bool pending = c.outPos < c.out.size();
    if (pending && std::chrono::duration_cast<std::chrono::milliseconds>(now - c.lastProgress).count() > cfg_.stallTimeoutMs) {
        ++dropped_;
        logLine("remote: cliente lento desconectado");
        return false;  // hopeless: never let it hold buffers forever
    }
    if (c.closing) {
        if (!pending && c.ctrl.empty()) return false;  // everything flushed: drop the connection
        if (now > c.closeDeadline) return false;
    }
    return true;
}

}  // namespace hb::remote
