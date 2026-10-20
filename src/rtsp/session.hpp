#pragma once
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "rtsp/auth.hpp"
#include "rtsp/message.hpp"
#include "rtsp/sdp.hpp"
#include "rtsp/types.hpp"

namespace hb::rtsp {

enum class TransportMode { TcpInterleaved, Udp };

enum class SessionState { Idle, Options, Describe, Setup, Play, Playing, Teardown, Closed, Failed };
const char* sessionStateName(SessionState s);

enum class FailureKind {
    None,
    Timeout,              // a request got no answer in time
    AuthFailed,           // 401 after sending credentials, or no credentials / no usable scheme
    NotFound,             // 404
    SessionLost,          // 454 Session Not Found
    Redirect,             // 3xx; redirectUrl() holds the target (not an error per se)
    BadResponse,          // malformed or unusable response
    ServerError,          // other 4xx/5xx
    NoSupportedMedia,     // no H.264/H.265 video in the SDP
    UnsupportedTransport, // 461 or server refused the requested transport
    ConnectionClosed,
    ProtocolError,        // unparsable byte stream
    Config,               // invalid configuration (bad URL, UDP data path missing...)
    ConnectFailed,        // TCP connect/send/recv error (set by RtspInput)
    NoData,               // watchdog: media stopped arriving (set by RtspInput)
    Internal,             // unexpected exception caught at the boundary (set by RtspInput)
};
const char* failureName(FailureKind k);

struct SessionConfig {
    std::string url;               // rtsp://[user:pass@]host[:port]/path (never logged unmasked)
    Credentials credentials;       // takes precedence over URL userinfo
    TransportMode transport = TransportMode::TcpInterleaved;
    std::uint16_t udpClientPort = 50000;  // RTP port; RTCP = +1 (UDP mode only)
    Millis requestTimeout{5000};
    Millis defaultSessionTimeout{60000};
    Millis maxKeepaliveInterval{30000};
    Millis minKeepaliveInterval{5000};
    std::string userAgent = "HBStudio/0.1";
    Authenticator::NonceGen cnonce;  // injectable for deterministic tests
};

// Everything the media pipeline needs after SETUP.
struct MediaInfo {
    Codec codec = Codec::H264;
    int payloadType = 96;
    int clockRate = 90000;
    std::vector<Bytes> paramSets;
    std::string controlUrl;
    double framerate = 0;
    int width = 0, height = 0;
    int rtpChannel = 0, rtcpChannel = 1;  // interleaved
    std::uint16_t serverRtpPort = 0, serverRtcpPort = 0;  // UDP
    std::uint32_t ssrc = 0;  // 0 = not announced
    std::optional<std::uint16_t> initialSeq;
    std::optional<std::uint32_t> initialRtpTime;
};

// RTSP client state machine. Pure logic: it never touches a socket. The owner feeds it received
// bytes, calls poll() with an injected clock, and writes takeOutput() to the connection.
//
//   Idle -> Options -> Describe -> Setup -> Play -> Playing -> Teardown -> Closed
//                       (401 -> retry with Authorization, at most twice per request)
//   any state -> Failed (failure(), failureMessage(); Redirect carries redirectUrl())
class RtspSession {
public:
    explicit RtspSession(SessionConfig cfg);

    void start(TimePoint now);
    void onBytes(ByteView data, TimePoint now);
    void poll(TimePoint now);
    void stop(TimePoint now);          // sends TEARDOWN when a session exists
    void onConnectionClosed();
    Bytes takeOutput();                // bytes to write to the TCP connection (RTSP text)

    SessionState state() const { return state_; }
    bool playing() const { return state_ == SessionState::Playing; }
    FailureKind failure() const { return failure_; }
    const std::string& failureMessage() const { return failureMsg_; }
    const std::string& redirectUrl() const { return redirect_; }
    const MediaInfo& media() const { return media_; }
    const std::string& sessionId() const { return sessionId_; }
    Millis keepaliveInterval() const { return keepaliveInterval_; }
    int keepalivesSent() const { return keepalivesSent_; }
    bool serverSupportsGetParameter() const { return hasGetParameter_; }

    // Called for every '$' frame (channel, payload) after the transport is established.
    std::function<void(std::uint8_t channel, ByteView payload)> onInterleaved;
    // Called once when PLAY is acknowledged.
    std::function<void()> onPlaying;

private:
    struct Pending {
        std::string method, uri;
        std::vector<std::pair<std::string, std::string>> extra;
        int cseq = 0;
        TimePoint sentAt{};
        int authTries = 0;
    };

    void sendMain(const std::string& method, const std::string& uri,
                  std::vector<std::pair<std::string, std::string>> extra, TimePoint now, int authTries = 0);
    void emit(Pending& p, bool withSession, TimePoint now);
    void onResponse(const Response& r, TimePoint now);
    void onMainResponse(const Response& r, TimePoint now);
    void onKeepaliveResponse(const Response& r, TimePoint now);
    void onServerRequest(const Request& rq);
    void handleDescribeOk(const Response& r, TimePoint now);
    void handleSetupOk(const Response& r, TimePoint now);
    void handlePlayOk(const Response& r, TimePoint now);
    void sendKeepalive(TimePoint now);
    void fail(FailureKind k, std::string msg);

    SessionConfig cfg_;
    Authenticator auth_;
    MessageParser parser_;
    Bytes out_;
    SessionState state_ = SessionState::Idle;
    FailureKind failure_ = FailureKind::None;
    std::string failureMsg_, redirect_;

    std::string requestUri_;  // from cfg url, no credentials
    std::string baseUrl_;     // Content-Base or request URI
    std::string sessionId_;
    Millis sessionTimeout_{60000};
    Millis keepaliveInterval_{30000};
    bool hasGetParameter_ = false;
    int cseq_ = 0;
    std::optional<Pending> pending_;
    std::optional<Pending> keepalive_;
    TimePoint nextKeepalive_{};
    int keepalivesSent_ = 0;
    MediaInfo media_;
    std::string sessionControl_;
};

}  // namespace hb::rtsp
