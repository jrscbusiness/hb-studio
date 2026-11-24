#pragma once
// One-shot HTTP listener bound to 127.0.0.1 only, for the OAuth loopback redirect (RFC 8252 7.3).
// Portable (BSD sockets / Winsock); tested on Linux against a local loopback connection.
#include <atomic>
#include <cstdint>
#include <string>

namespace hb::chat {

class LoopbackServer {
public:
    LoopbackServer() = default;
    ~LoopbackServer();
    LoopbackServer(const LoopbackServer&) = delete;
    LoopbackServer& operator=(const LoopbackServer&) = delete;

    // Binds 127.0.0.1:<ephemeral port>. false on failure.
    bool listen();
    int port() const { return port_; }
    std::string redirectUri() const { return "http://127.0.0.1:" + std::to_string(port_); }

    // Accepts connections until one carries "code=" or "error=" in its request line (others, such as
    // /favicon.ico, get a 404), answers it with `responseHtml`, and returns the raw request head.
    // Gives up after timeoutMs or when `cancel` becomes true (checked every ~200 ms). Request heads
    // are capped at 8 KiB. Empty string on timeout/cancel.
    std::string waitForRedirect(int timeoutMs, const std::string& responseHtml, const std::atomic<bool>* cancel);
    void close();

private:
    std::intptr_t fd_ = -1;
    int port_ = 0;
};

}  // namespace hb::chat
