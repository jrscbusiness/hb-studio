#pragma once
// HTTP transport abstraction for the AI client. Real implementation: WinHTTP (http_win.cpp, Windows
// only, NOT VALIDATED on real hardware). Elsewhere makeDefaultAiHttp() returns a transport that
// always fails with a clear message, so portable code still links; tests use fakes.
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace hb::ai {

struct HttpRequest {
    std::string method = "POST";
    std::string url;  // http:// (loopback only, enforced by the client) or https:// (certificate verified)
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    std::chrono::milliseconds timeout{30000};
    // Streaming: when set and the status is 2xx, body chunks go here instead of being accumulated;
    // return false to abort the transfer. For non-2xx statuses the body is accumulated normally.
    std::function<bool(const std::string&)> onData;
};

struct HttpResponse {
    int status = 0;                           // 0 when the exchange itself failed
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;  // at least Retry-After when present
    std::string transportError;               // non-empty => transport failure (no usable status)
    bool timedOut = false;
};

class IAiHttp {
public:
    virtual ~IAiHttp() = default;
    virtual HttpResponse send(const HttpRequest& request) = 0;  // never throws
};

std::shared_ptr<IAiHttp> makeDefaultAiHttp();
inline constexpr std::size_t kMaxAiResponseBytes = 8 * 1024 * 1024;

}  // namespace hb::ai
