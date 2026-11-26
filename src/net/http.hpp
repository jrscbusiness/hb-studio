#pragma once
// The one HTTP client abstraction of HB Studio. Real implementation: WinHTTP (http_win.cpp, Windows
// only). Elsewhere makeWinHttpClient() returns a client that always fails with a clear error, and
// tests use FakeHttpClient (http_fake.hpp). Modules keep their own small interfaces and adapt them
// onto this one (see docs/REDE.md).
//
// Security invariants (not configurable): HTTPS certificates are always verified (no SECURITY_FLAG_IGNORE_*
// anywhere), redirects never downgrade https -> http, and a request is refused up front when its URL does
// not parse as strict http/https.
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace hb::net {

using Headers = std::vector<std::pair<std::string, std::string>>;

enum class RedirectPolicy {
    FollowNoDowngrade,  // follow (WinHTTP's default limit), never https -> http
    Never,              // hand 3xx back to the caller as-is
};

enum class ProxyMode {
    System,     // WINHTTP_ACCESS_TYPE_DEFAULT_PROXY
    Automatic,  // WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY (WPAD/PAC), falling back to System if the session cannot open
    None,       // WINHTTP_ACCESS_TYPE_NO_PROXY (LAN devices)
};

enum class Overflow {
    Fail,      // body larger than maxResponseBytes: error kind TooLarge
    Truncate,  // keep the first maxResponseBytes and stop reading; not an error
};

enum class ErrorKind {
    None,
    InvalidRequest,  // bad URL, scheme refused (httpsOnly), request body too large
    Transport,       // DNS / connect / TLS / read failure
    Timeout,
    TooLarge,        // see Overflow::Fail
    Cancelled,       // body source, sink or progress callback asked to stop
};

// Finer classification of Transport/Timeout errors, so modules can show specific messages without
// knowing WinHTTP error numbers.
enum class Failure {
    Other,
    Timeout,
    Tls,              // certificate / secure-channel failure
    NameNotResolved,
    CannotConnect,
    ConnectionError,  // connection dropped
    RedirectFailed,   // redirect refused (downgrade, loop, limit)
};

struct Timeouts {
    int resolveMs = 30000, connectMs = 30000, sendMs = 30000, receiveMs = 30000;  // 0 = no limit (WinHTTP semantics)
    static Timeouts all(int ms) { return Timeouts{ms, ms, ms, ms}; }
};

// Returned by a body source to abort the upload.
inline constexpr std::size_t kBodyAbort = std::numeric_limits<std::size_t>::max();

struct HttpRequest {
    std::string method = "GET";
    std::string url;  // http:// or https://
    Headers headers;
    std::string body;  // used when bodySource is empty

    // Streaming upload: when set, `bodySourceLength` bytes are produced by calling the source with a
    // buffer to fill; it returns the number of bytes written (0 = end) or kBodyAbort. `body` is ignored.
    std::function<std::size_t(char* buffer, std::size_t capacity)> bodySource;
    std::uint64_t bodySourceLength = 0;
    std::size_t bodySourceChunk = 32 * 1024;

    Timeouts timeouts;
    // Wall-clock budget for reading a *buffered* body (not applied while streaming into onData); 0 = none.
    std::chrono::milliseconds bufferedBodyDeadline{0};

    RedirectPolicy redirects = RedirectPolicy::FollowNoDowngrade;
    int maxRedirects = 0;    // 0 = WinHTTP's default limit
    bool httpsOnly = false;  // refuse http:// URLs (and, with redirects, cross-scheme hops stay non-downgrading)

    // Response handling. Bytes that are not streamed are accumulated in HttpResponse::body, capped at
    // maxResponseBytes (0 = unlimited).
    std::size_t maxResponseBytes = 0;
    Overflow overflow = Overflow::Fail;
    // Streaming sink: body chunks of responses with status in [sinkMinStatus, sinkMaxStatus] go here
    // instead of being accumulated; `total` is Content-Length or 0. Return false to abort (Cancelled).
    std::function<bool(const char* data, std::size_t size, std::uint64_t total)> onData;
    int sinkMinStatus = 200, sinkMaxStatus = 299;
    bool readBodyOutsideSink = true;  // false: statuses outside the sink range return without reading the body
    // Progress/cancel: called after every body chunk with (bytes received so far, Content-Length or 0).
    // Return false to cancel the transfer.
    std::function<bool(std::uint64_t received, std::uint64_t total)> onProgress;
};

struct HttpResponse {
    int status = 0;  // 0 when the exchange itself failed before a status line
    Headers headers;  // all response headers (names as sent)
    std::string body;
    std::uint64_t contentLength = 0;  // 0 when absent
    std::uint64_t bytesReceived = 0;  // body bytes seen (streamed or buffered), before any truncation
    ErrorKind error = ErrorKind::None;
    std::string errorText;      // short technical description, never contains secrets
    Failure failure = Failure::Other;
    std::string errorStage;     // e.g. "WinHttpConnect" (empty when not applicable)
    unsigned long nativeError = 0;  // GetLastError() of the failing call, 0 if none
    bool ok() const { return error == ErrorKind::None; }
    // Case-insensitive lookup; returns "" when absent.
    std::string header(const std::string& name) const;
};

class IHttpClient {
public:
    virtual ~IHttpClient() = default;
    virtual HttpResponse send(const HttpRequest& request) = 0;  // never throws; thread-safe
};

struct HttpClientOptions {
    std::string userAgent = "HBStudio/1.0";
    ProxyMode proxy = ProxyMode::System;
    // Windows only: minimum TLS protocols (WINHTTP_OPTION_SECURE_PROTOCOLS mask); 0 = OS default.
    unsigned long secureProtocols = 0;
};

// WinHTTP on Windows; elsewhere a client whose every send() fails with ErrorKind::Transport
// ("indisponivel nesta plataforma"). Never returns nullptr.
std::shared_ptr<IHttpClient> makeWinHttpClient(const HttpClientOptions& options = {});

// Validation shared by the WinHTTP client and the fake: returns ErrorKind::None when the request may be sent,
// otherwise fills `error`. Parsed URL is returned through `url`.
struct Url;
ErrorKind validateRequest(const HttpRequest& request, Url& url, std::string& errorText);

}  // namespace hb::net
