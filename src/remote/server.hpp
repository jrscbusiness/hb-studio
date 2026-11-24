#pragma once
// Embedded HTTP + WebSocket server for the phone/tablet remote. Portable (built on the
// hb::rtsp::ITcpStream / TcpListener abstraction), one I/O thread + one command-executor thread.
//
// Threading / latency contract (the point of the design):
//   * Only the I/O thread ever touches sockets. broadcastState()/broadcastThumb() (called from the
//     video feeder thread) just store the LATEST value under a short mutex - they never block on a
//     socket and never allocate per client. Each client remembers which sequence number it has
//     seen, so a slow client simply skips intermediate states/thumbnails ("drop on overflow").
//   * Control replies are bounded per client; a client whose socket makes no progress for
//     stallTimeoutMs (or whose control queue overflows) is disconnected.
//   * Commands run on a separate executor thread, so a slow host command (e.g. starting a
//     recording) never delays WebSocket I/O.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "remote/auth.hpp"
#include "remote/commands.hpp"
#include "rtsp/tcp_stream.hpp"

namespace hb::remote {

struct ServerConfig {
    std::vector<std::string> bindAddresses = {"127.0.0.1"};  // IPv4 literals; the service passes the LAN interfaces
    std::uint16_t port = 0;                                   // 0 = ephemeral
    std::size_t maxConnections = 12;
    std::size_t maxUnauthenticated = 8;
    std::size_t maxPerIp = 6;
    std::size_t maxHttpHeadBytes = 8192;
    std::size_t maxInboundMessageBytes = 4096;  // commands are tiny; anything bigger is hostile
    std::size_t maxCtrlQueue = 64;              // queued control frames per client before it is dropped
    int httpTimeoutMs = 5000;                   // slowloris: complete request head within this
    int authTimeoutMs = 5000;                   // WebSocket must authenticate/pair within this
    int pingIntervalMs = 10000;
    int idleTimeoutMs = 30000;                  // no inbound frame at all (pong/ping/cmd) -> close
    int stallTimeoutMs = 4000;                  // pending output without any progress -> close
    int stateMinIntervalMs = 100;               // <= 10 Hz
    int thumbBytesPerSec = 100 * 1024;          // per-client thumbnail bandwidth cap
    std::size_t maxThumbBytes = 64 * 1024;
    int commandsPerSec = 60;                    // per-client command rate limit (burst = 1 s)
    bool lanPeersOnly = true;                   // refuse peers that are not private/loopback/link-local IPv4
    CommandLimits commandLimits;
    std::string pageHtml;                       // served at GET /
};

class RemoteServer {
public:
    using CommandSink = std::function<void(const std::string& hostCommand)>;
    using LogSink = std::function<void(const std::string& line)>;  // never receives secrets

    RemoteServer(ServerConfig cfg, AuthManager& auth, CommandSink sink, LogSink log = {});
    ~RemoteServer();
    RemoteServer(const RemoteServer&) = delete;
    RemoteServer& operator=(const RemoteServer&) = delete;

    bool start(std::string& error);
    void stop();  // idempotent; joins threads, closes every connection
    bool running() const { return running_.load(); }
    std::uint16_t port() const { return port_; }
    const std::vector<std::string>& boundAddresses() const { return bound_; }

    // Host -> remote. Thread-safe, non-blocking. State is dropped if identical to the last one.
    void broadcastState(const std::string& json);
    // kind: 0 = PREVIEW, 1 = PROGRAM. Ignored if no client opted in or the JPEG is too large.
    void broadcastThumb(int kind, const std::vector<std::uint8_t>& jpeg);
    bool wantsThumbs() const { return thumbClients_.load() > 0; }

    struct ConnectedDevice {
        std::uint64_t sessionId = 0;
        bool thumbs = false;
    };
    std::vector<ConnectedDevice> connectedDevices() const;
    std::size_t clientCount() const { return clientCount_.load(); }
    std::uint64_t droppedClients() const { return dropped_.load(); }  // slow/hopeless/abusive disconnects

private:
    struct Client;
    using Clock = std::chrono::steady_clock;
    void ioLoop();
    void execLoop();
    void acceptAll(Clock::time_point now);
    bool service(Client& c, Clock::time_point now, bool& activity);
    bool pumpRead(Client& c, Clock::time_point now, bool& activity);
    bool pumpWrite(Client& c, Clock::time_point now, bool& activity);
    bool fillNext(Client& c, Clock::time_point now);
    bool handleHttp(Client& c, Clock::time_point now);
    bool handleWsMessage(Client& c, const std::string& text, Clock::time_point now);
    void queueText(Client& c, const std::string& json);
    void queueClose(Client& c, std::uint16_t code, const char* reason, Clock::time_point now);
    void respondAndClose(Client& c, int status, const std::string& body, Clock::time_point now);
    void publishLive(const std::vector<std::unique_ptr<Client>>& clients);
    void logLine(const std::string& s) const {
        if (log_) log_(s);
    }

    ServerConfig cfg_;
    AuthManager& auth_;
    CommandSink sink_;
    LogSink log_;
    std::vector<std::unique_ptr<hb::rtsp::TcpListener>> listeners_;
    std::vector<std::string> bound_;
    std::uint16_t port_ = 0;
    std::atomic<bool> running_{false};
    std::thread io_, exec_;

    // Latest host state / thumbnails (written by the feeder thread, read by the I/O thread).
    mutable std::mutex mailMu_;
    std::string latestState_;
    std::uint64_t stateSeq_ = 0;
    std::vector<std::uint8_t> latestThumb_[2];
    std::uint64_t thumbSeq_[2] = {0, 0};

    mutable std::mutex liveMu_;
    std::vector<ConnectedDevice> live_;
    std::atomic<int> thumbClients_{0};
    std::atomic<std::size_t> clientCount_{0};
    std::atomic<std::uint64_t> dropped_{0};

    std::mutex execMu_;
    std::condition_variable execCv_;
    std::deque<std::string> execQ_;
    bool execQuit_ = false;
};

}  // namespace hb::remote
