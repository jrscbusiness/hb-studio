#pragma once
// Minimal, strict HTTP/1.1 request-head parser + response builders for the remote-control server.
// Only GET without a body is supported. Pure logic, hostile-input safe (bounded, never throws).
#include <cstddef>
#include <cstdint>
#include <utility>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace hb::remote::http {

struct Request {
    std::string method;
    std::string target;                          // raw request-target
    std::string path;                            // target without the query string
    std::map<std::string, std::string> headers;  // lower-case names; repeated headers are rejected
    const std::string* header(const std::string& lowerName) const {
        auto it = headers.find(lowerName);
        return it == headers.end() ? nullptr : &it->second;
    }
};

class RequestParser {
public:
    explicit RequestParser(std::size_t maxHeadBytes = 8192) : max_(maxHeadBytes) {}
    enum class State { NeedMore, Done, Error };
    // Feeds bytes. Done: request() is valid and leftover() holds bytes that followed the head
    // (a WebSocket client may legally pipeline nothing before our 101, so callers treat leftover
    // as protocol data). Error: errorStatus() is the HTTP status to answer with.
    State feed(const std::uint8_t* data, std::size_t len);
    const Request& request() const { return req_; }
    int errorStatus() const { return status_; }
    const std::string& leftover() const { return leftover_; }

private:
    State parseHead(std::string_view head);
    std::size_t max_;
    std::string buf_;
    Request req_;
    std::string leftover_;
    State state_ = State::NeedMore;
    int status_ = 400;
};

// Case-insensitive: does a comma separated header value contain `token`?
bool headerHasToken(std::string_view value, std::string_view token);
// Host header acceptable? Only IP literals ("192.168.0.5:8787", "[::1]:80") and "localhost" -
// a DNS name would allow DNS-rebinding attacks from a malicious web page.
bool hostIsIpLiteralOrLocalhost(std::string_view hostHeader);
// Origin header equals "http://" + Host (same-origin; browsers always send Origin on WebSocket).
bool originMatchesHost(std::string_view origin, std::string_view host);

struct Response {
    int status = 200;
    std::string contentType = "text/plain; charset=utf-8";
    std::string body;
    std::vector<std::pair<std::string, std::string>> extraHeaders;
};
// Serialises with Content-Length, Connection: close and the common hardening headers
// (CSP, nosniff, no-store, X-Frame-Options, Referrer-Policy).
std::string serialize(const Response& r);
const char* reason(int status);

}  // namespace hb::remote::http
