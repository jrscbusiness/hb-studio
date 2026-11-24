#pragma once
// RemoteService: the application-facing facade of the remote control. Owns the AuthManager and the
// RemoteServer, decides which interfaces/port to bind, and produces the status JSON for the
// desktop CONFIG card. OFF by default and never persisted as "on": every app start begins disabled.
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "remote/auth.hpp"
#include "remote/server.hpp"
#include "remote/state.hpp"

namespace hb::remote {

struct ServiceOptions {
    std::vector<std::string> bindOverride;  // empty = this machine's LAN interface addresses (tests: {"127.0.0.1"})
    std::uint16_t preferredPort = 8787;     // tries this and the next 9, then any free port (0 = always any)
    ServerConfig server;                    // limits; bindAddresses/port/pageHtml are filled by the service
    AuthConfig auth;
};

class RemoteService {
public:
    RemoteService(RemoteServer::CommandSink sink, RemoteServer::LogSink log = {}, ServiceOptions opt = {});
    ~RemoteService();

    bool enable(std::string& error);
    void disable();
    bool enabled() const;

    // Desktop UI messages: "remote|on", "remote|off", "remote|newpin", "remote|revoke", "remote|kick|<id>",
    // "remote|status". Returns a toast text ("" = none). Never throws.
    std::string handleUi(const std::string& message);
    std::string statusJson();

    // Host -> remote (cheap no-ops while disabled / without clients).
    bool wantsState() const;
    void pushState(const RemoteState& s);
    bool wantsThumbs() const;
    void pushThumb(int kind, const std::vector<std::uint8_t>& jpeg);

    AuthManager& auth() { return auth_; }
    std::uint16_t port() const;

private:
    ServiceOptions opt_;
    RemoteServer::CommandSink sink_;
    RemoteServer::LogSink log_;
    AuthManager auth_;
    mutable std::mutex mu_;
    std::unique_ptr<RemoteServer> server_;
    std::string lastError_;
    std::vector<std::string> urls_;
};

}  // namespace hb::remote
