#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "streaming/stream_stats.hpp"
#include "streaming/transport.hpp"

// Reconnect-with-backoff around an IStreamTransport, portable and fully testable with a fake
// transport and an injected clock. StreamSender owns one per destination and drives it from its
// single thread: poll() before each loop iteration, sendVideo()/sendAudio() for media.
//
//  - connection lost (write failed / transport stopped publishing) -> state Waiting, next attempt
//    after an exponentially growing delay (1 s, 2 s, 4 s ... capped at 30 s, +-20% jitter);
//  - permanent failures (server refused the key/app, certificate rejected, bad URL, protocol not
//    available) -> GaveUp immediately: hammering a refusing server only risks a platform ban;
//  - after maxAttempts consecutive failures -> GaveUp;
//  - the attempt counter resets once a connection has stayed up for stableAfterMs, so a flapping
//    link keeps backing off instead of retrying fast forever;
//  - timestamps are re-based to 0 on every new connection (a fresh publish session starts at 0), and
//    generation() changes so the caller can re-send the AVC/AAC sequence headers and wait for a
//    keyframe.
namespace hb::streaming {

struct BackoffConfig {
    int baseMs = 1000;
    int maxMs = 30000;
    double factor = 2.0;
    int maxAttempts = 20;      // consecutive failures before giving up (~8 min of trying at the cap)
    int jitterPercent = 20;
};

class ReconnectBackoff {
public:
    using Rand01 = std::function<double()>;   // uniform in [0,1); injectable for tests
    explicit ReconnectBackoff(const BackoffConfig& cfg = {}, Rand01 rnd = nullptr);
    // Registers one more failure and returns the delay before the next attempt, or -1 when exhausted.
    int nextDelayMs();
    void reset() { attempts_ = 0; }
    int attempts() const { return attempts_; }

private:
    BackoffConfig cfg_;
    Rand01 rnd_;
    int attempts_ = 0;
};

class ReconnectingPublisher {
public:
    struct Options {
        std::string url;
        std::string streamKey;
        int connectTimeoutMs = 8000;
        int stableAfterMs = 30000;
        BackoffConfig backoff;
    };
    using TransportFactory = std::function<std::unique_ptr<IStreamTransport>()>;
    using NowMs = std::function<std::int64_t()>;
    enum class State { Idle, Connected, Waiting, GaveUp };

    // `stats` may be null. `now` defaults to the steady clock. `rnd` feeds the backoff jitter.
    ReconnectingPublisher(Options opts, TransportFactory factory, StreamStats* stats = nullptr, NowMs now = nullptr,
                          ReconnectBackoff::Rand01 rnd = nullptr);
    ~ReconnectingPublisher();

    // True when connected and ready for sendVideo/sendAudio. When disconnected and the backoff wait has
    // elapsed it (re)connects, blocking up to the transport's timeout.
    bool poll();
    bool sendVideo(std::uint32_t timestampMs, const std::vector<std::uint8_t>& body);
    bool sendAudio(std::uint32_t timestampMs, const std::vector<std::uint8_t>& body);
    void stop();

    State state() const { return state_; }
    bool connected() const { return state_ == State::Connected; }
    bool gaveUp() const { return state_ == State::GaveUp; }
    // Changes at every successful (re)connect. Re-send codec headers / wait for a keyframe on change.
    unsigned generation() const { return generation_; }
    int attempts() const { return backoff_.attempts(); }
    std::string lastError() const { return lastError_; }
    // Milliseconds until the next connection attempt (0 when due or not waiting).
    std::int64_t msUntilRetry() const;

private:
    bool send(bool video, std::uint32_t ts, const std::vector<std::uint8_t>& body);
    void lost(const std::string& why);
    void scheduleRetry();

    Options opts_;
    TransportFactory factory_;
    StreamStats* stats_;
    NowMs now_;
    ReconnectBackoff backoff_;
    std::unique_ptr<IStreamTransport> transport_;
    State state_ = State::Idle;
    unsigned generation_ = 0;
    bool everConnected_ = false;
    std::int64_t nextAttemptMs_ = 0;
    std::int64_t connectedAtMs_ = 0;
    bool haveBase_ = false;
    std::uint32_t tsBase_ = 0;
    std::string lastError_;
};

}  // namespace hb::streaming
