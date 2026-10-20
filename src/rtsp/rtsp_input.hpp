#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "core/hardware_profile.hpp"
#include "core/logger.hpp"
#include "rtsp/session.hpp"
#include "rtsp/stream_receiver.hpp"
#include "rtsp/tcp_stream.hpp"

namespace hb::rtsp {

enum class InputState { Stopped, Connecting, Online, Error, Reconnecting };
const char* inputStateName(InputState s);

enum class HealthLevel { Good, Degraded, Bad };
const char* healthLevelName(HealthLevel l);

struct RtspInputConfig {
    std::string url;                // may carry user:pass@; never logged unmasked
    Credentials credentials;        // overrides URL userinfo
    core::LatencyMode latency = core::LatencyMode::Balanced;
    TransportMode transport = TransportMode::TcpInterleaved;  // UDP data path: NOT IMPLEMENTED

    Millis connectTimeout{5000};
    Millis requestTimeout{5000};
    Millis firstDataTimeout{5000};  // PLAY acknowledged but no RTP yet
    Millis noDataTimeout{3000};     // watchdog: no RTP packets while playing
    Millis noFrameTimeout{15000};   // RTP flows but no decodable access unit (e.g. waiting for IDR)

    // ARCHITECTURE section 10 / acceptance criterion: 1 s -> 2 -> 4 ... ceiling 15 s, with jitter.
    // (These deliberately do not follow latencyParams().reconnect*, which differs per mode.)
    Millis reconnectInitial{1000};
    Millis reconnectMax{15000};
    double backoffJitter = 0.2;     // +-20 %
    Millis stableAfter{10000};      // Online this long resets the backoff
    bool retryOnAuthFailure = false;  // wrong password: do not hammer the camera (lockouts)
    int maxRedirects = 5;

    std::size_t queueCapacity = 0;  // 0 = 4 x latencyParams().decodeQueueFrames

    // Health thresholds.
    double degradedLossPercent = 2.0;
    Millis degradedFrameAge{1500};
};

struct InputDeps {
    TcpStreamFactory streamFactory;               // default: makeSocketTcpStream
    std::shared_ptr<core::Logger> logger;         // optional
    std::function<double()> random01;             // [0,1); default: std::mt19937
    std::function<std::string()> cnonce;          // Digest client nonce; default random hex
};

struct InputHealth {
    InputState state = InputState::Stopped;
    HealthLevel level = HealthLevel::Bad;
    std::string maskedUrl;
    FailureKind lastFailure = FailureKind::None;
    std::string lastError;
    double bitrateKbps = 0;
    double lossPercent = 0;
    std::uint32_t jitterMs = 0;
    std::uint64_t accessUnits = 0;
    std::uint64_t keyframes = 0;
    std::uint64_t queueDropped = 0;
    std::uint64_t reconnects = 0;     // reconnect cycles since start()
    unsigned attempt = 0;             // current backoff exponent
    std::optional<Millis> sinceLastFrame;
    Millis onlineFor{0};
    RtpStats rtp;
    DepacketizerStats depacketizer;
};

// RTSP video provider. Single "tick" function driven by an injected clock => deterministic tests;
// startThread() wraps it in a worker thread with the real clock for production use.
// Failures never propagate: every error ends in Error/Reconnecting with exponential backoff.
//
// Callbacks run on the tick thread while the internal tick lock is held: do not call
// start/stop/tick from inside them (health() and popAccessUnit() are safe).
class RtspInput {
public:
    using StateCallback = std::function<void(InputState from, InputState to, const std::string& reason)>;
    using AuCallback = std::function<void(const AccessUnit&)>;

    RtspInput(RtspInputConfig cfg, InputDeps deps = {});
    ~RtspInput();
    RtspInput(const RtspInput&) = delete;
    RtspInput& operator=(const RtspInput&) = delete;

    void setStateCallback(StateCallback cb);
    // If set, access units go to the callback instead of the internal queue.
    void setAccessUnitCallback(AuCallback cb);

    void start(TimePoint now);
    void stop(TimePoint now);   // best-effort TEARDOWN, then Stopped
    void tick(TimePoint now);

    void startThread();
    void stopThread();

    std::optional<AccessUnit> popAccessUnit();
    InputState state() const;
    InputHealth health(TimePoint now) const;

private:
    enum class Phase { Idle, Backoff, TcpConnecting, Handshake, Streaming, Permanent };

    void beginAttempt(TimePoint now);
    void onTcpConnected(TimePoint now);
    void pump(TimePoint now);
    bool flushOutput(TimePoint now);
    void onInterleaved(std::uint8_t channel, ByteView payload, TimePoint now);
    void handleSessionFailure(TimePoint now);
    void fail(FailureKind kind, const std::string& msg, TimePoint now);
    void closeConnection();
    void setState(InputState s, const std::string& reason);
    void goOnline(TimePoint t);
    Millis backoffDelay(unsigned attempt);
    void publishSnapshot(TimePoint now);
    void log(core::Level lvl, const std::string& msg);
    void waitActivity();

    RtspInputConfig cfg_;
    InputDeps deps_;
    std::string url_;  // current target (changes on redirect)

    mutable std::mutex tickMu_;
    std::unique_ptr<ITcpStream> stream_;
    std::unique_ptr<RtspSession> session_;
    std::unique_ptr<StreamReceiver> receiver_;
    Bytes pendingOut_;
    Bytes rxBuf_;

    bool running_ = false;
    TimePoint curNow_{};  // clock value of the tick in progress (for session callbacks)
    Phase phase_ = Phase::Idle;
    TimePoint nextAttempt_{}, attemptStart_{}, playAt_{}, onlineSince_{};
    std::optional<TimePoint> lastRtpAt_, lastAuAt_;
    unsigned attempt_ = 0;
    int redirects_ = 0;
    unsigned attemptsStarted_ = 0;
    std::uint64_t reconnects_ = 0;
    std::uint64_t totalAus_ = 0, totalKeyframes_ = 0;
    bool stableCounted_ = false;
    FailureKind lastFailure_ = FailureKind::None;
    std::string lastError_;

    StateCallback stateCb_;
    AuCallback auCb_;
    InputState state_ = InputState::Stopped;

    mutable std::mutex statusMu_;
    InputHealth snapshot_;
    std::optional<TimePoint> snapLastAu_;
    TimePoint snapOnlineSince_{};
    InputState snapState_ = InputState::Stopped;

    std::mutex queueMu_;
    AccessUnitQueue queue_;

    std::thread thread_;
    std::atomic<bool> threadStop_{false};
};

}  // namespace hb::rtsp
