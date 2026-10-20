#include "rtsp/rtsp_input.hpp"

#include <algorithm>
#include <cmath>
#include <random>

#include "rtsp/url.hpp"

namespace hb::rtsp {

const char* inputStateName(InputState s) {
    switch (s) {
        case InputState::Stopped: return "STOPPED";
        case InputState::Connecting: return "CONNECTING";
        case InputState::Online: return "ONLINE";
        case InputState::Error: return "ERROR";
        case InputState::Reconnecting: return "RECONNECTING";
    }
    return "?";
}

const char* healthLevelName(HealthLevel l) {
    switch (l) {
        case HealthLevel::Good: return "GOOD";
        case HealthLevel::Degraded: return "DEGRADED";
        case HealthLevel::Bad: return "BAD";
    }
    return "?";
}

namespace {
constexpr std::size_t kReadChunk = 64 * 1024;
constexpr int kMaxReadsPerTick = 64;
constexpr std::size_t kMaxPendingOut = 1024 * 1024;

std::mt19937& rng() {
    static thread_local std::mt19937 g{std::random_device{}()};
    return g;
}
}  // namespace

RtspInput::RtspInput(RtspInputConfig cfg, InputDeps deps)
    : cfg_(std::move(cfg)),
      deps_(std::move(deps)),
      url_(cfg_.url),
      queue_(cfg_.queueCapacity ? cfg_.queueCapacity
                                : std::size_t(core::latencyParams(cfg_.latency).decodeQueueFrames) * 4,
             core::latencyParams(cfg_.latency).dropOldestOnOverflow) {
    if (!deps_.streamFactory) deps_.streamFactory = [] { return makeSocketTcpStream(); };
    if (!deps_.random01)
        deps_.random01 = [] { return std::uniform_real_distribution<double>(0.0, 1.0)(rng()); };
    if (!deps_.cnonce)
        deps_.cnonce = [] {
            static const char* hex = "0123456789abcdef";
            std::string s;
            for (int i = 0; i < 16; ++i) s += hex[rng()() & 15];
            return s;
        };
    rxBuf_.resize(kReadChunk);
    snapshot_.maskedUrl = maskUrl(cfg_.url);
}

RtspInput::~RtspInput() { stopThread(); }

void RtspInput::setStateCallback(StateCallback cb) {
    std::lock_guard<std::mutex> l(tickMu_);
    stateCb_ = std::move(cb);
}
void RtspInput::setAccessUnitCallback(AuCallback cb) {
    std::lock_guard<std::mutex> l(tickMu_);
    auCb_ = std::move(cb);
}

void RtspInput::log(core::Level lvl, const std::string& msg) {
    if (!deps_.logger) return;
    try {
        deps_.logger->log(lvl, "rtsp", msg);
    } catch (...) {
    }
}

void RtspInput::setState(InputState s, const std::string& reason) {
    if (s == state_) return;
    InputState from = state_;
    state_ = s;
    log(s == InputState::Error ? core::Level::Warning : core::Level::Info,
        std::string(inputStateName(from)) + " -> " + inputStateName(s) + (reason.empty() ? "" : " (" + reason + ")"));
    if (stateCb_) {
        try {
            stateCb_(from, s, reason);
        } catch (...) {
        }
    }
}

InputState RtspInput::state() const {
    std::lock_guard<std::mutex> l(statusMu_);
    return snapState_;
}

void RtspInput::start(TimePoint now) {
    std::lock_guard<std::mutex> l(tickMu_);
    if (running_) return;
    running_ = true;
    url_ = cfg_.url;
    attempt_ = 0;
    redirects_ = 0;
    attemptsStarted_ = 0;
    reconnects_ = 0;
    stableCounted_ = false;
    lastFailure_ = FailureKind::None;
    lastError_.clear();
    phase_ = Phase::Backoff;
    nextAttempt_ = now;
    setState(InputState::Connecting, "start");
    publishSnapshot(now);
}

void RtspInput::stop(TimePoint now) {
    std::lock_guard<std::mutex> l(tickMu_);
    if (!running_) return;
    running_ = false;
    if (session_ && stream_ && session_->playing()) {
        try {
            session_->stop(now);
            flushOutput(now);  // best effort TEARDOWN
        } catch (...) {
        }
    }
    closeConnection();
    phase_ = Phase::Idle;
    setState(InputState::Stopped, "stopped");
    publishSnapshot(now);
}

void RtspInput::closeConnection() {
    if (stream_) {
        try {
            stream_->close();
        } catch (...) {
        }
    }
    stream_.reset();
    session_.reset();
    receiver_.reset();
    pendingOut_.clear();
    lastRtpAt_.reset();
    {
        std::lock_guard<std::mutex> l(queueMu_);
        queue_.clear();
    }
}

Millis RtspInput::backoffDelay(unsigned attempt) {
    double base = double(cfg_.reconnectInitial.count()) * std::pow(2.0, std::min(attempt, 20u));
    base = std::min(base, double(cfg_.reconnectMax.count()));
    double j = std::clamp(cfg_.backoffJitter, 0.0, 0.9);
    double factor = 1.0 + j * (2.0 * std::clamp(deps_.random01(), 0.0, 1.0) - 1.0);
    double d = std::min(base * factor, double(cfg_.reconnectMax.count()));
    return Millis(static_cast<long long>(std::max(d, 0.0)));
}

void RtspInput::fail(FailureKind kind, const std::string& msg, TimePoint now) {
    log(core::Level::Warning, "stream " + maskUrl(url_) + " failed: " + failureName(kind) + ": " + msg);
    closeConnection();
    lastFailure_ = kind;
    lastError_ = msg;
    setState(InputState::Error, std::string(failureName(kind)) + ": " + msg);
    bool permanent = kind == FailureKind::Config || (kind == FailureKind::AuthFailed && !cfg_.retryOnAuthFailure);
    if (permanent) {
        phase_ = Phase::Permanent;
        publishSnapshot(now);
        return;
    }
    Millis d = backoffDelay(attempt_++);
    nextAttempt_ = now + d;
    phase_ = Phase::Backoff;
    ++reconnects_;
    stableCounted_ = false;
    setState(InputState::Reconnecting, "retry in " + std::to_string(d.count()) + " ms");
    publishSnapshot(now);
}

void RtspInput::beginAttempt(TimePoint now) {
    ++attemptsStarted_;
    attemptStart_ = now;
    if (cfg_.transport == TransportMode::Udp) {
        fail(FailureKind::Config, "UDP transport data path is not implemented (use TCP interleaved)", now);
        return;
    }
    auto u = parseRtspUrl(url_);
    if (!u) {
        fail(FailureKind::Config, "invalid RTSP URL (only rtsp:// is supported)", now);
        return;
    }
    stream_ = deps_.streamFactory ? deps_.streamFactory() : nullptr;
    if (!stream_) {
        fail(FailureKind::ConnectFailed, "could not create a TCP stream", now);
        return;
    }
    if (attemptsStarted_ > 1 && state_ != InputState::Reconnecting) setState(InputState::Reconnecting, "attempt");
    log(core::Level::Info, "connecting to " + maskUrl(url_));
    IoStatus st = stream_->connect(u->host, u->port);
    if (st == IoStatus::Error) {
        fail(FailureKind::ConnectFailed, "connect: " + stream_->lastError(), now);
    } else if (st == IoStatus::Ok) {
        onTcpConnected(now);
    } else {
        phase_ = Phase::TcpConnecting;
    }
}

void RtspInput::onTcpConnected(TimePoint now) {
    SessionConfig sc;
    sc.url = url_;
    sc.credentials = cfg_.credentials;
    sc.transport = cfg_.transport;
    sc.requestTimeout = cfg_.requestTimeout;
    sc.cnonce = deps_.cnonce;
    session_ = std::make_unique<RtspSession>(sc);
    session_->onInterleaved = [this](std::uint8_t ch, ByteView p) { onInterleaved(ch, p, curNow_); };
    session_->onPlaying = [this] {
        playAt_ = curNow_;
        phase_ = Phase::Streaming;
    };
    phase_ = Phase::Handshake;
    curNow_ = now;
    session_->start(now);
    flushOutput(now);
}

bool RtspInput::flushOutput(TimePoint now) {
    if (!stream_ || !session_) return true;
    Bytes o = session_->takeOutput();
    pendingOut_.insert(pendingOut_.end(), o.begin(), o.end());
    if (pendingOut_.size() > kMaxPendingOut) {
        fail(FailureKind::ConnectFailed, "send buffer overflow (peer not reading)", now);
        return false;
    }
    std::size_t off = 0;
    while (off < pendingOut_.size()) {
        std::size_t sent = 0;
        IoStatus st = stream_->send(pendingOut_.data() + off, pendingOut_.size() - off, sent);
        if (st == IoStatus::Ok) {
            off += sent;
        } else if (st == IoStatus::WouldBlock) {
            break;
        } else {
            std::string err = stream_->lastError();
            if (running_) fail(FailureKind::ConnectFailed, "send: " + err, now);
            return false;
        }
    }
    pendingOut_.erase(pendingOut_.begin(), pendingOut_.begin() + static_cast<std::ptrdiff_t>(off));
    return true;
}

void RtspInput::onInterleaved(std::uint8_t channel, ByteView payload, TimePoint now) {
    if (!session_) return;
    const MediaInfo& m = session_->media();
    if (!receiver_) {
        StreamReceiverConfig rc;
        rc.codec = m.codec;
        rc.payloadType = m.payloadType;
        rc.clockRate = static_cast<std::uint32_t>(m.clockRate);
        rc.jitter = JitterConfig::forMode(cfg_.latency, cfg_.transport == TransportMode::TcpInterleaved, rc.clockRate);
        rc.paramSets = m.paramSets;
        receiver_ = std::make_unique<StreamReceiver>(rc);
        receiver_->setSink([this](AccessUnit&& au) {
            lastAuAt_ = au.completedAt;
            ++totalAus_;
            if (au.keyframe) ++totalKeyframes_;
            if (state_ != InputState::Online) goOnline(au.completedAt);
            if (auCb_) {
                try {
                    auCb_(au);
                } catch (...) {
                }
            } else {
                std::lock_guard<std::mutex> l(queueMu_);
                queue_.push(std::move(au));
            }
        });
    }
    if (channel == m.rtpChannel) {
        lastRtpAt_ = now;
        receiver_->onRtp(payload, now);
    } else if (channel == m.rtcpChannel) {
        receiver_->onRtcp(payload, now);
    }
}

void RtspInput::goOnline(TimePoint t) {
    onlineSince_ = t;
    setState(InputState::Online, "receiving video");
}

void RtspInput::handleSessionFailure(TimePoint now) {
    FailureKind k = session_->failure();
    std::string msg = session_->failureMessage();
    if (k == FailureKind::Redirect) {
        if (redirects_ >= cfg_.maxRedirects) {
            fail(FailureKind::BadResponse, "too many redirects", now);
            return;
        }
        ++redirects_;
        std::string target = session_->redirectUrl();
        // Keep our credentials; the redirect target is used as-is otherwise.
        log(core::Level::Info, "redirected to " + maskUrl(target));
        closeConnection();
        url_ = target;
        beginAttempt(now);
        return;
    }
    fail(k, msg, now);
}

void RtspInput::pump(TimePoint now) {
    curNow_ = now;
    if (!flushOutput(now)) return;
    bool closed = false;
    for (int i = 0; i < kMaxReadsPerTick && stream_; ++i) {
        std::size_t got = 0;
        IoStatus st = stream_->recv(rxBuf_.data(), rxBuf_.size(), got);
        if (st == IoStatus::Ok) {
            session_->onBytes(ByteView(rxBuf_.data(), got), now);
            if (session_->state() == SessionState::Failed) break;
        } else if (st == IoStatus::WouldBlock) {
            break;
        } else if (st == IoStatus::Closed) {
            closed = true;
            break;
        } else {
            fail(FailureKind::ConnectFailed, "recv: " + stream_->lastError(), now);
            return;
        }
    }
    if (closed && session_->state() != SessionState::Failed) session_->onConnectionClosed();
    if (session_->state() == SessionState::Failed) {
        handleSessionFailure(now);
        return;
    }
    if (!session_) return;
    session_->poll(now);
    if (session_->state() == SessionState::Failed) {
        handleSessionFailure(now);
        return;
    }
    if (receiver_) {
        receiver_->poll(now);
        if (session_->playing()) {
            if (auto rr = receiver_->takeReceiverReport(now)) {
                Bytes fr = frameInterleaved(static_cast<std::uint8_t>(session_->media().rtcpChannel), *rr);
                pendingOut_.insert(pendingOut_.end(), fr.begin(), fr.end());
            }
        }
    }
    if (!flushOutput(now)) return;

    if (session_->playing()) {
        // Watchdogs.
        TimePoint ref = lastRtpAt_ ? *lastRtpAt_ : playAt_;
        Millis limit = lastRtpAt_ ? cfg_.noDataTimeout : cfg_.firstDataTimeout;
        if (now - ref >= limit) {
            fail(FailureKind::NoData, lastRtpAt_ ? "no RTP data received" : "no RTP data after PLAY", now);
            return;
        }
        TimePoint auRef = lastAuAt_ ? *lastAuAt_ : playAt_;
        if (lastRtpAt_ && now - auRef >= cfg_.noFrameTimeout) {
            fail(FailureKind::NoData, "RTP flows but no decodable frames", now);
            return;
        }
        if (receiver_ && receiver_->snapshot(now).byeReceived) {
            fail(FailureKind::ConnectionClosed, "server sent RTCP BYE", now);
            return;
        }
        if (state_ == InputState::Online && !stableCounted_ && now - onlineSince_ >= cfg_.stableAfter) {
            attempt_ = 0;
            redirects_ = 0;
            stableCounted_ = true;
        }
    }
}

void RtspInput::tick(TimePoint now) {
    std::lock_guard<std::mutex> l(tickMu_);
    if (!running_) return;
    try {
        switch (phase_) {
            case Phase::Backoff:
                if (now >= nextAttempt_) beginAttempt(now);
                break;
            case Phase::TcpConnecting: {
                IoStatus st = stream_ ? stream_->pollConnect() : IoStatus::Error;
                if (st == IoStatus::Ok) onTcpConnected(now);
                else if (st == IoStatus::Error)
                    fail(FailureKind::ConnectFailed, "connect: " + (stream_ ? stream_->lastError() : std::string("no stream")), now);
                else if (now - attemptStart_ >= cfg_.connectTimeout)
                    fail(FailureKind::Timeout, "TCP connect timed out", now);
                break;
            }
            case Phase::Handshake:
            case Phase::Streaming:
                if (stream_ && session_) pump(now);
                break;
            case Phase::Permanent:
            case Phase::Idle:
                break;
        }
    } catch (const std::exception& e) {
        fail(FailureKind::Internal, std::string("internal error: ") + e.what(), now);
    } catch (...) {
        fail(FailureKind::Internal, "internal error", now);
    }
    publishSnapshot(now);
}

void RtspInput::publishSnapshot(TimePoint now) {
    InputHealth h;
    h.state = state_;
    h.maskedUrl = maskUrl(cfg_.url);
    h.lastFailure = lastFailure_;
    h.lastError = lastError_;
    h.accessUnits = totalAus_;
    h.keyframes = totalKeyframes_;
    h.reconnects = reconnects_;
    h.attempt = attempt_;
    {
        std::lock_guard<std::mutex> l(queueMu_);
        h.queueDropped = queue_.dropped();
    }
    if (receiver_) {
        auto s = receiver_->snapshot(now);
        h.bitrateKbps = s.bitrateBps / 1000.0;
        h.lossPercent = s.rtp.lossPercent;
        h.jitterMs = static_cast<std::uint32_t>(double(s.rtp.jitter) * 1000.0 / 90000.0);
        h.rtp = s.rtp;
        h.depacketizer = s.depacketizer;
    }
    std::lock_guard<std::mutex> l(statusMu_);
    // Keep the last media statistics across the gap between a failure and the next connection.
    if (!receiver_) {
        h.bitrateKbps = 0;
        h.rtp = snapshot_.rtp;
        h.depacketizer = snapshot_.depacketizer;
        h.lossPercent = snapshot_.lossPercent;
        h.jitterMs = snapshot_.jitterMs;
    }
    snapshot_ = h;
    snapLastAu_ = lastAuAt_;
    snapOnlineSince_ = onlineSince_;
    snapState_ = state_;
}

InputHealth RtspInput::health(TimePoint now) const {
    std::lock_guard<std::mutex> l(statusMu_);
    InputHealth h = snapshot_;
    h.state = snapState_;
    if (snapLastAu_) h.sinceLastFrame = std::chrono::duration_cast<Millis>(now - *snapLastAu_);
    if (snapState_ == InputState::Online) h.onlineFor = std::chrono::duration_cast<Millis>(now - snapOnlineSince_);
    if (snapState_ != InputState::Online) {
        h.level = HealthLevel::Bad;
    } else if (h.lossPercent >= cfg_.degradedLossPercent ||
               (h.sinceLastFrame && *h.sinceLastFrame >= cfg_.degradedFrameAge)) {
        h.level = HealthLevel::Degraded;
    } else {
        h.level = HealthLevel::Good;
    }
    return h;
}

std::optional<AccessUnit> RtspInput::popAccessUnit() {
    std::lock_guard<std::mutex> l(queueMu_);
    return queue_.pop();
}

void RtspInput::waitActivity() {
    ITcpStream* s = stream_.get();
    if (s) {
        try {
            if (s->waitReadable(2)) return;
        } catch (...) {
        }
        return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
}

void RtspInput::startThread() {
    if (thread_.joinable()) return;
    threadStop_ = false;
    start(std::chrono::steady_clock::now());
    thread_ = std::thread([this] {
        while (!threadStop_) {
            tick(std::chrono::steady_clock::now());
            waitActivity();
        }
    });
}

void RtspInput::stopThread() {
    if (!thread_.joinable()) return;
    threadStop_ = true;
    thread_.join();
    stop(std::chrono::steady_clock::now());
}

}  // namespace hb::rtsp
