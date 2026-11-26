#include "streaming/reconnecting_publisher.hpp"

#include <algorithm>
#include <cmath>
#include <random>

namespace hb::streaming {

ReconnectBackoff::ReconnectBackoff(const BackoffConfig& cfg, Rand01 rnd) : cfg_(cfg), rnd_(std::move(rnd)) {
    if (!rnd_) {
        auto gen = std::make_shared<std::mt19937>(std::random_device{}());
        rnd_ = [gen] { return std::uniform_real_distribution<double>(0.0, 1.0)(*gen); };
    }
}

int ReconnectBackoff::nextDelayMs() {
    if (attempts_ >= cfg_.maxAttempts) return -1;
    double d = cfg_.baseMs * std::pow(cfg_.factor, attempts_);
    d = std::min<double>(d, cfg_.maxMs);
    ++attempts_;
    const double j = std::clamp(cfg_.jitterPercent, 0, 90) / 100.0;
    d *= 1.0 + (rnd_() * 2.0 - 1.0) * j;
    return std::max(0, static_cast<int>(std::lround(d)));
}

ReconnectingPublisher::ReconnectingPublisher(Options opts, TransportFactory factory, StreamStats* stats, NowMs now,
                                             ReconnectBackoff::Rand01 rnd)
    : opts_(std::move(opts)),
      factory_(std::move(factory)),
      stats_(stats),
      now_(now ? std::move(now) : NowMs(steadyNowMs)),
      backoff_(opts_.backoff, std::move(rnd)) {}

ReconnectingPublisher::~ReconnectingPublisher() { stop(); }

void ReconnectingPublisher::stop() {
    if (transport_) {
        transport_->stop();
        transport_.reset();
    }
    state_ = State::Idle;
}

std::int64_t ReconnectingPublisher::msUntilRetry() const {
    if (state_ != State::Waiting) return 0;
    return std::max<std::int64_t>(0, nextAttemptMs_ - now_());
}

void ReconnectingPublisher::scheduleRetry() {
    const int d = backoff_.nextDelayMs();
    if (d < 0) {
        state_ = State::GaveUp;
        lastError_ += " Desistindo após " + std::to_string(backoff_.attempts()) + " tentativas.";
        if (stats_) stats_->setLastError(lastError_);
        return;
    }
    nextAttemptMs_ = now_() + d;
    state_ = State::Waiting;
}

void ReconnectingPublisher::lost(const std::string& why) {
    lastError_ = why;
    if (stats_) stats_->setLastError(why);
    if (transport_) {
        transport_->stop();
        transport_.reset();
    }
    scheduleRetry();
}

bool ReconnectingPublisher::poll() {
    const std::int64_t now = now_();
    if (state_ == State::Connected) {
        if (backoff_.attempts() > 0 && now - connectedAtMs_ >= opts_.stableAfterMs) backoff_.reset();
        return true;
    }
    if (state_ == State::GaveUp) return false;
    if (state_ == State::Waiting && now < nextAttemptMs_) return false;

    transport_ = factory_ ? factory_() : nullptr;
    if (!transport_) {
        lastError_ = "Protocolo de transmissão indisponível.";
        if (stats_) stats_->setLastError(lastError_);
        state_ = State::GaveUp;
        return false;
    }
    if (transport_->open(opts_.url, opts_.streamKey, opts_.connectTimeoutMs) && transport_->isPublishing()) {
        state_ = State::Connected;
        ++generation_;
        connectedAtMs_ = now_();
        haveBase_ = false;
        if (everConnected_ && stats_) stats_->onReconnect();
        everConnected_ = true;
        return true;
    }
    lastError_ = transport_->lastError().empty() ? "Falha ao conectar." : transport_->lastError();
    if (stats_) stats_->setLastError(lastError_);
    const bool permanent = transport_->permanentFailure();
    transport_->stop();
    transport_.reset();
    if (permanent) {
        state_ = State::GaveUp;
        return false;
    }
    scheduleRetry();
    return false;
}

bool ReconnectingPublisher::send(bool video, std::uint32_t ts, const std::vector<std::uint8_t>& body) {
    if (state_ != State::Connected || !transport_) return false;
    if (!haveBase_) {
        haveBase_ = true;
        tsBase_ = ts;
    }
    const std::uint32_t rel = ts >= tsBase_ ? ts - tsBase_ : 0;
    const bool ok = video ? transport_->sendVideo(rel, body) : transport_->sendAudio(rel, body);
    if (!ok || !transport_->isPublishing()) {
        lost(transport_->lastError().empty() ? "Conexão perdida." : "Conexão perdida: " + transport_->lastError());
        return false;
    }
    if (stats_) stats_->onSent(video, body.size());
    return true;
}

bool ReconnectingPublisher::sendVideo(std::uint32_t ts, const std::vector<std::uint8_t>& body) { return send(true, ts, body); }
bool ReconnectingPublisher::sendAudio(std::uint32_t ts, const std::vector<std::uint8_t>& body) { return send(false, ts, body); }

}  // namespace hb::streaming
