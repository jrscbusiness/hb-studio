#include "rtsp/session.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>

#include "rtsp/url.hpp"

namespace hb::rtsp {

const char* sessionStateName(SessionState s) {
    switch (s) {
        case SessionState::Idle: return "Idle";
        case SessionState::Options: return "Options";
        case SessionState::Describe: return "Describe";
        case SessionState::Setup: return "Setup";
        case SessionState::Play: return "Play";
        case SessionState::Playing: return "Playing";
        case SessionState::Teardown: return "Teardown";
        case SessionState::Closed: return "Closed";
        case SessionState::Failed: return "Failed";
    }
    return "?";
}

const char* failureName(FailureKind k) {
    switch (k) {
        case FailureKind::None: return "None";
        case FailureKind::Timeout: return "Timeout";
        case FailureKind::AuthFailed: return "AuthFailed";
        case FailureKind::NotFound: return "NotFound";
        case FailureKind::SessionLost: return "SessionLost";
        case FailureKind::Redirect: return "Redirect";
        case FailureKind::BadResponse: return "BadResponse";
        case FailureKind::ServerError: return "ServerError";
        case FailureKind::NoSupportedMedia: return "NoSupportedMedia";
        case FailureKind::UnsupportedTransport: return "UnsupportedTransport";
        case FailureKind::ConnectionClosed: return "ConnectionClosed";
        case FailureKind::ProtocolError: return "ProtocolError";
        case FailureKind::Config: return "Config";
        case FailureKind::ConnectFailed: return "ConnectFailed";
        case FailureKind::NoData: return "NoData";
        case FailureKind::Internal: return "Internal";
    }
    return "?";
}

namespace {
std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}
// Splits "a=b;c;d=e" into tokens (lower-cased keys kept with raw values).
std::vector<std::string> splitSemi(std::string_view s) {
    std::vector<std::string> out;
    std::size_t p = 0;
    while (p <= s.size()) {
        auto semi = s.find(';', p);
        auto seg = trim(s.substr(p, semi == std::string_view::npos ? std::string_view::npos : semi - p));
        if (!seg.empty()) out.emplace_back(seg);
        if (semi == std::string_view::npos) break;
        p = semi + 1;
    }
    return out;
}
bool parseUInt(std::string_view s, unsigned long long& v, int base = 10) {
    s = trim(s);
    if (s.empty() || s.size() > 12) return false;
    std::string t(s);
    char* end = nullptr;
    v = std::strtoull(t.c_str(), &end, base);
    return end && *end == 0;
}
// "a-b" or "a"
bool parseRange(std::string_view s, unsigned& a, unsigned& b) {
    unsigned long long x, y;
    auto dash = s.find('-');
    if (!parseUInt(s.substr(0, dash), x) || x > 65535) return false;
    a = static_cast<unsigned>(x);
    b = a + 1;
    if (dash != std::string_view::npos) {
        if (!parseUInt(s.substr(dash + 1), y) || y > 65535) return false;
        b = static_cast<unsigned>(y);
    }
    return true;
}
}  // namespace

RtspSession::RtspSession(SessionConfig cfg)
    : cfg_(std::move(cfg)), auth_(Credentials{}, cfg_.cnonce) {
    if (auto u = parseRtspUrl(cfg_.url)) {
        requestUri_ = u->requestUri();
        if (cfg_.credentials.empty()) cfg_.credentials = u->userinfo;
        auth_ = Authenticator(cfg_.credentials, cfg_.cnonce);
    }
    baseUrl_ = requestUri_;
    sessionTimeout_ = cfg_.defaultSessionTimeout;
}

void RtspSession::fail(FailureKind k, std::string msg) {
    if (state_ == SessionState::Failed) return;
    state_ = SessionState::Failed;
    failure_ = k;
    failureMsg_ = std::move(msg);
    pending_.reset();
    keepalive_.reset();
}

Bytes RtspSession::takeOutput() {
    Bytes o;
    o.swap(out_);
    return o;
}

void RtspSession::start(TimePoint now) {
    if (state_ != SessionState::Idle) return;
    if (requestUri_.empty()) {
        fail(FailureKind::Config, "invalid RTSP URL");
        return;
    }
    state_ = SessionState::Options;
    sendMain("OPTIONS", requestUri_, {}, now);
}

void RtspSession::emit(Pending& p, bool withSession, TimePoint now) {
    Request rq;
    rq.method = p.method;
    rq.uri = p.uri;
    p.cseq = ++cseq_;
    p.sentAt = now;
    rq.headers.add("CSeq", std::to_string(p.cseq));
    rq.headers.add("User-Agent", cfg_.userAgent);
    for (auto& [k, v] : p.extra) rq.headers.add(k, v);
    if (withSession && !sessionId_.empty()) rq.headers.add("Session", sessionId_);
    std::string authHdr = auth_.header(p.method, p.uri);
    if (!authHdr.empty()) rq.headers.add("Authorization", authHdr);
    std::string s = serialize(rq);
    out_.insert(out_.end(), s.begin(), s.end());
}

void RtspSession::sendMain(const std::string& method, const std::string& uri,
                           std::vector<std::pair<std::string, std::string>> extra, TimePoint now, int authTries) {
    Pending p;
    p.method = method;
    p.uri = uri;
    p.extra = std::move(extra);
    p.authTries = authTries;
    emit(p, method != "OPTIONS" && method != "DESCRIBE", now);
    pending_ = std::move(p);
}

void RtspSession::onBytes(ByteView data, TimePoint now) {
    if (state_ == SessionState::Failed || state_ == SessionState::Closed) return;
    parser_.feed(data);
    MessageParser::Item item;
    for (;;) {
        auto st = parser_.next(item);
        if (st == MessageParser::Status::NeedMore) break;
        if (st == MessageParser::Status::Error) {
            fail(FailureKind::ProtocolError, "RTSP stream error: " + parser_.error());
            break;
        }
        if (auto* r = std::get_if<Response>(&item)) onResponse(*r, now);
        else if (auto* q = std::get_if<Request>(&item)) onServerRequest(*q);
        else if (auto* f = std::get_if<InterleavedFrame>(&item)) {
            if (onInterleaved && (state_ == SessionState::Play || state_ == SessionState::Playing))
                onInterleaved(f->channel, ByteView(f->data));
        }
        if (state_ == SessionState::Failed || state_ == SessionState::Closed) break;
    }
}

void RtspSession::onServerRequest(const Request& rq) {
    Response r;
    if (rq.method == "OPTIONS" || rq.method == "GET_PARAMETER" || rq.method == "SET_PARAMETER") {
        r.status = 200;
        r.reason = "OK";
    } else {
        r.status = 501;
        r.reason = "Not Implemented";
    }
    if (auto c = rq.headers.get("CSeq")) r.headers.add("CSeq", *c);
    std::string s = serialize(r);
    out_.insert(out_.end(), s.begin(), s.end());
}

void RtspSession::onResponse(const Response& r, TimePoint now) {
    int c = r.cseq();
    if (keepalive_ && c == keepalive_->cseq) onKeepaliveResponse(r, now);
    else if (pending_ && (c == pending_->cseq || c == -1)) onMainResponse(r, now);
    // otherwise: stale/unsolicited response, ignore.
}

void RtspSession::onKeepaliveResponse(const Response& r, TimePoint now) {
    Pending kp = *keepalive_;
    keepalive_.reset();
    if (r.status == 454) {
        fail(FailureKind::SessionLost, "server reports session not found (454)");
    } else if (r.status == 401 && kp.authTries < 1 && auth_.setChallenge(r.headers.getAll("WWW-Authenticate"))) {
        kp.authTries++;
        emit(kp, true, now);
        keepalive_ = std::move(kp);
    }
    // any other status: the request reached the server, which is all a keepalive needs
}

void RtspSession::onMainResponse(const Response& r, TimePoint now) {
    Pending p = *pending_;
    const int st = r.status;

    if (st == 401) {
        if (p.authTries >= 2) {
            fail(FailureKind::AuthFailed, "authentication rejected (401)");
            return;
        }
        if (!auth_.hasCredentials()) {
            fail(FailureKind::AuthFailed, "server requires authentication but no credentials were given");
            return;
        }
        bool wasAuthenticated = auth_.hasChallenge();
        if (!auth_.setChallenge(r.headers.getAll("WWW-Authenticate"))) {
            fail(FailureKind::AuthFailed, "no supported authentication scheme offered");
            return;
        }
        // Second 401 in a row is only retried when the nonce was merely stale.
        if (p.authTries >= 1 && wasAuthenticated && !auth_.lastChallengeStale()) {
            fail(FailureKind::AuthFailed, "authentication rejected (401), wrong credentials?");
            return;
        }
        sendMain(p.method, p.uri, p.extra, now, p.authTries + 1);
        return;
    }
    if (st >= 300 && st < 400) {
        auto loc = r.headers.get("Location");
        if (!loc || !parseRtspUrl(*loc)) {
            fail(FailureKind::BadResponse, "redirect without a usable Location");
            return;
        }
        redirect_ = *loc;
        fail(FailureKind::Redirect, "redirected");
        return;
    }
    if (st == 404) { fail(FailureKind::NotFound, "stream not found (404)"); return; }
    if (st == 454) { fail(FailureKind::SessionLost, "session not found (454)"); return; }
    if (st == 461) { fail(FailureKind::UnsupportedTransport, "transport not supported (461)"); return; }
    if (st < 200 || st >= 300) {
        if (p.method == "OPTIONS" && st != 401) {
            // Some cameras do not implement OPTIONS; go on to DESCRIBE.
            pending_.reset();
            state_ = SessionState::Describe;
            sendMain("DESCRIBE", requestUri_, {{"Accept", "application/sdp"}}, now);
            return;
        }
        fail(FailureKind::ServerError, "server answered " + std::to_string(st) + " " + r.reason + " to " + p.method);
        return;
    }

    pending_.reset();
    switch (state_) {
        case SessionState::Options: {
            if (auto pub = r.headers.get("Public")) hasGetParameter_ = lower(*pub).find("get_parameter") != std::string::npos;
            state_ = SessionState::Describe;
            sendMain("DESCRIBE", requestUri_, {{"Accept", "application/sdp"}}, now);
            break;
        }
        case SessionState::Describe: handleDescribeOk(r, now); break;
        case SessionState::Setup: handleSetupOk(r, now); break;
        case SessionState::Play: handlePlayOk(r, now); break;
        case SessionState::Teardown:
            state_ = SessionState::Closed;
            break;
        default: break;
    }
}

void RtspSession::handleDescribeOk(const Response& r, TimePoint now) {
    auto sdp = parseSdp(r.body);
    if (!sdp) {
        fail(FailureKind::BadResponse, "DESCRIBE returned no valid SDP");
        return;
    }
    auto sel = selectVideo(*sdp);
    if (!sel) {
        fail(FailureKind::NoSupportedMedia, "no H.264/H.265 video track in SDP");
        return;
    }
    if (sel->media->proto.find("SAVP") != std::string::npos) {
        fail(FailureKind::NoSupportedMedia, "SRTP (RTP/SAVP) is not supported");
        return;
    }
    if (sel->codec == Codec::H265 && sel->fmtp.spropMaxDonDiff > 0) {
        fail(FailureKind::NoSupportedMedia, "H.265 with DON interleaving is not supported");
        return;
    }
    if (auto cb = r.headers.get("Content-Base")) baseUrl_ = *cb;
    else if (auto cl = r.headers.get("Content-Location")) baseUrl_ = resolveControl(requestUri_, *cl);
    else baseUrl_ = requestUri_;
    sessionControl_ = sdp->control;

    media_ = MediaInfo{};
    media_.codec = sel->codec;
    media_.payloadType = sel->payloadType;
    media_.clockRate = sel->clockRate;
    media_.paramSets = sel->fmtp.paramSets;
    media_.framerate = sel->media->framerate;
    media_.width = sel->media->width;
    media_.height = sel->media->height;
    std::string base = sessionControl_.empty() || sessionControl_ == "*" ? baseUrl_ : resolveControl(baseUrl_, sessionControl_);
    // RFC 2326 C.1.1: a media control that is absolute wins; relative ones resolve against the base.
    media_.controlUrl = sel->media->control.empty() ? base : resolveControl(baseUrl_, sel->media->control);

    std::string transport;
    if (cfg_.transport == TransportMode::TcpInterleaved) {
        transport = "RTP/AVP/TCP;unicast;interleaved=0-1";
    } else {
        transport = "RTP/AVP;unicast;client_port=" + std::to_string(cfg_.udpClientPort) + "-" +
                    std::to_string(cfg_.udpClientPort + 1);
    }
    state_ = SessionState::Setup;
    sendMain("SETUP", media_.controlUrl, {{"Transport", transport}}, now);
}

void RtspSession::handleSetupOk(const Response& r, TimePoint now) {
    if (auto s = r.headers.get("Session")) {
        auto parts = splitSemi(*s);
        if (!parts.empty()) sessionId_ = parts[0];
        for (std::size_t i = 1; i < parts.size(); ++i) {
            auto eq = parts[i].find('=');
            if (eq != std::string::npos && lower(parts[i].substr(0, eq)) == "timeout") {
                unsigned long long t;
                if (parseUInt(parts[i].substr(eq + 1), t) && t >= 2 && t <= 86400) sessionTimeout_ = Millis(t * 1000);
            }
        }
    }
    auto tr = r.headers.get("Transport");
    if (!tr) {
        fail(FailureKind::BadResponse, "SETUP response without Transport header");
        return;
    }
    bool tcp = false, interleavedSeen = false;
    for (auto& tok : splitSemi(*tr)) {
        std::string lk = lower(tok);
        auto eq = tok.find('=');
        std::string key = lower(tok.substr(0, eq));
        std::string_view val = eq == std::string::npos ? std::string_view{} : std::string_view(tok).substr(eq + 1);
        if (lk.rfind("rtp/avp/tcp", 0) == 0) tcp = true;
        else if (key == "interleaved") {
            unsigned a, b;
            if (parseRange(val, a, b) && a < 256 && b < 256) {
                media_.rtpChannel = static_cast<int>(a);
                media_.rtcpChannel = static_cast<int>(b);
                interleavedSeen = true;
            }
        } else if (key == "server_port") {
            unsigned a, b;
            if (parseRange(val, a, b)) {
                media_.serverRtpPort = static_cast<std::uint16_t>(a);
                media_.serverRtcpPort = static_cast<std::uint16_t>(b);
            }
        } else if (key == "ssrc") {
            unsigned long long v;
            if (parseUInt(val, v, 16) && v <= 0xffffffffull) media_.ssrc = static_cast<std::uint32_t>(v);
        }
    }
    if (cfg_.transport == TransportMode::TcpInterleaved && !(tcp || interleavedSeen)) {
        fail(FailureKind::UnsupportedTransport, "server did not accept RTP over TCP");
        return;
    }
    if (cfg_.transport == TransportMode::Udp && tcp) {
        fail(FailureKind::UnsupportedTransport, "server answered TCP to a UDP SETUP");
        return;
    }
    state_ = SessionState::Play;
    sendMain("PLAY", baseUrl_, {{"Range", "npt=0.000-"}}, now);
}

void RtspSession::handlePlayOk(const Response& r, TimePoint now) {
    if (auto info = r.headers.get("RTP-Info")) {
        for (auto& tok : splitSemi(*info)) {
            auto eq = tok.find('=');
            if (eq == std::string::npos) continue;
            std::string key = lower(tok.substr(0, eq));
            unsigned long long v;
            if (key == "seq" && parseUInt(tok.substr(eq + 1), v) && v <= 0xffff) media_.initialSeq = static_cast<std::uint16_t>(v);
            else if (key == "rtptime" && parseUInt(tok.substr(eq + 1), v) && v <= 0xffffffffull)
                media_.initialRtpTime = static_cast<std::uint32_t>(v);
        }
    }
    auto half = sessionTimeout_ / 2;
    keepaliveInterval_ = std::clamp(half, cfg_.minKeepaliveInterval, cfg_.maxKeepaliveInterval);
    nextKeepalive_ = now + keepaliveInterval_;
    state_ = SessionState::Playing;
    if (onPlaying) onPlaying();
}

void RtspSession::sendKeepalive(TimePoint now) {
    Pending p;
    p.method = hasGetParameter_ ? "GET_PARAMETER" : "OPTIONS";
    p.uri = baseUrl_;
    emit(p, true, now);
    keepalive_ = std::move(p);
    ++keepalivesSent_;
    nextKeepalive_ = now + keepaliveInterval_;
}

void RtspSession::poll(TimePoint now) {
    switch (state_) {
        case SessionState::Options:
        case SessionState::Describe:
        case SessionState::Setup:
        case SessionState::Play:
            if (pending_ && now - pending_->sentAt >= cfg_.requestTimeout)
                fail(FailureKind::Timeout, "no response to " + pending_->method + " within timeout");
            break;
        case SessionState::Playing:
            if (keepalive_ && now - keepalive_->sentAt >= cfg_.requestTimeout) keepalive_.reset();  // data watchdog decides
            if (!keepalive_ && now >= nextKeepalive_) sendKeepalive(now);
            break;
        case SessionState::Teardown:
            if (pending_ && now - pending_->sentAt >= Millis(1000)) state_ = SessionState::Closed;
            break;
        default: break;
    }
}

void RtspSession::stop(TimePoint now) {
    if (state_ == SessionState::Playing || state_ == SessionState::Play || state_ == SessionState::Setup) {
        if (!sessionId_.empty()) {
            keepalive_.reset();
            state_ = SessionState::Teardown;
            sendMain("TEARDOWN", baseUrl_, {}, now);
            return;
        }
    }
    if (state_ != SessionState::Failed) state_ = SessionState::Closed;
}

void RtspSession::onConnectionClosed() {
    if (state_ == SessionState::Teardown) state_ = SessionState::Closed;
    else if (state_ != SessionState::Closed && state_ != SessionState::Failed)
        fail(FailureKind::ConnectionClosed, "connection closed by peer");
}

}  // namespace hb::rtsp
