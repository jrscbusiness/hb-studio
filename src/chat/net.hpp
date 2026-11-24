#pragma once
// Transport abstractions for the chat module. The chat module deliberately does NOT depend on
// hb_onvif; these are its own tiny interfaces (HTTPS client, text WebSocket, secret store,
// backoff) so tests inject fakes and Windows-only code stays in separate files.
#include <chrono>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "chat/types.hpp"

namespace hb::chat {

struct HttpRequest {
    std::string method = "GET";
    std::string url;  // https:// (http:// only for loopback tests)
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    int timeoutMs = 8000;
};

struct HttpResponse {
    bool transportOk = false;  // false: DNS/TLS/connect/timeout failure (see `error`)
    int status = 0;            // any HTTP status, including 4xx/5xx, is transportOk == true
    std::string body;
    std::string error;
};

// Implementations must be usable from several threads (one source per thread).
class IChatHttp {
public:
    virtual ~IChatHttp() = default;
    virtual HttpResponse send(const HttpRequest& request) = 0;
};

// Text WebSocket client (used for Twitch IRC over wss).
class IChatSocket {
public:
    enum class Recv { Message, Timeout, Closed };
    virtual ~IChatSocket() = default;
    virtual bool connect(const std::string& url, std::string& error) = 0;
    virtual bool sendText(const std::string& text) = 0;
    // Waits up to timeoutMs (0 = poll) for one complete text frame.
    virtual Recv receive(std::string& out, int timeoutMs) = 0;
    virtual void close() = 0;
};

// Real implementations exist on Windows only (WinHTTP, http_win.cpp / socket_win.cpp). Elsewhere
// they return nullptr: the app simply cannot reach these services there (tests use fakes).
std::unique_ptr<IChatHttp> makeDefaultChatHttp();
std::unique_ptr<IChatSocket> makeDefaultChatSocket();

// Secrets (OAuth refresh tokens, API/Page tokens, client secrets). Values are never logged and are
// never sent to the UI; only maskTail() hints are. The Windows implementation encrypts per user
// with DPAPI. (Own tiny interface on purpose: the coordinator unifies the secret stores later.)
class ISecretStore {
public:
    virtual ~ISecretStore() = default;
    virtual bool put(const std::string& key, const std::string& value) = 0;
    virtual bool get(const std::string& key, std::string& out) const = 0;
    virtual bool erase(const std::string& key) = 0;
};

class MemorySecretStore : public ISecretStore {
public:
    bool put(const std::string& key, const std::string& value) override;
    bool get(const std::string& key, std::string& out) const override;
    bool erase(const std::string& key) override;

private:
    mutable std::vector<std::pair<std::string, std::string>> items_;
};

// DPAPI-encrypted files under `dir` (Windows). Returns nullptr on other platforms.
std::unique_ptr<ISecretStore> makeDpapiSecretStore(const std::string& dir);

// Exponential backoff with an injectable clock (the caller passes `now`). No jitter: one client.
class Backoff {
public:
    Backoff(Millis base = Millis(2000), Millis cap = Millis(60000)) : base_(base), cap_(cap) {}
    // Records a failure and returns the delay to wait before the next attempt.
    Millis onFailure() {
        Millis d = base_;
        for (int i = 0; i < failures_ && d < cap_; ++i) d *= 2;
        if (d > cap_) d = cap_;
        if (failures_ < 30) ++failures_;
        return d;
    }
    void onSuccess() { failures_ = 0; }
    int failures() const { return failures_; }

private:
    Millis base_, cap_;
    int failures_ = 0;
};

}  // namespace hb::chat
