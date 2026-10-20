#pragma once
#include <chrono>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "onvif/result.hpp"

namespace hb::onvif {

struct HttpRequest {
    std::string method = "POST";
    std::string url;  // http:// or https://
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    std::chrono::milliseconds timeout{5000};  // total budget for the exchange
};

struct HttpResponse {
    int status = 0;
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;
};

// Transport abstraction. Implementations must be thread-safe or be used from one thread.
// A transport failure returns an Error (Transport / Timeout); any HTTP status, including 4xx/5xx,
// is a successful HttpResponse (the caller interprets it).
class IHttpClient {
public:
    virtual ~IHttpClient() = default;
    virtual Result<HttpResponse> send(const HttpRequest& request) = 0;
};

// Real client for the current platform: WinHTTP on Windows (http_client_win.cpp, NOT VALIDATED),
// plain-HTTP BSD sockets on POSIX (http_client_posix.cpp; no TLS). Never returns nullptr.
std::unique_ptr<IHttpClient> makeDefaultHttpClient();

// Upper bound for response bodies, protects against hostile/broken devices.
inline constexpr std::size_t kMaxHttpResponseBytes = 4 * 1024 * 1024;

}  // namespace hb::onvif
