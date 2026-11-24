#pragma once
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace hb::marketplace {

// Network abstraction for the marketplace. Contract for every implementation:
//   * https only (an http:// URL must be refused), certificate verification ON, no way to turn it off;
//   * redirects may be followed but never downgraded to http;
//   * hard size cap enforced WHILE receiving (stop reading as soon as maxBytes is exceeded);
//   * a total time budget;
//   * never throws; any failure is reported in the result (messages are pt-BR, no secrets).
struct FetchRequest {
    std::string url;
    std::uint64_t maxBytes = 2 * 1024 * 1024;
    std::chrono::milliseconds timeout{15000};
    std::string ifNoneMatch;    // ETag of the cached copy, empty = unconditional
};

struct FetchResponse {
    bool transportOk = false;   // false: no HTTP exchange happened (offline, DNS, TLS, timeout, too large)
    std::string error;          // only when !transportOk
    int status = 0;             // HTTP status when transportOk (200, 304, 404, ...)
    std::string body;
    std::string etag;           // response ETag header, if any
};

struct FileRequest {
    std::string url;
    std::string destPathUtf8;   // written to "<dest>.part", renamed to <dest> only when complete
    std::uint64_t maxBytes = 0; // required > 0
    std::chrono::milliseconds timeout{600000};
};

struct FileResult {
    bool ok = false;
    bool cancelled = false;
    std::string error;          // pt-BR, when !ok
    int status = 0;
    std::uint64_t bytes = 0;
};

// Called as data arrives with (bytesReceived, bytesExpected or 0 if unknown). Return false to cancel.
using ProgressFn = std::function<bool(std::uint64_t received, std::uint64_t total)>;

class IHttpDownloader {
public:
    virtual ~IHttpDownloader() = default;
    virtual FetchResponse fetch(const FetchRequest& request) = 0;
    virtual FileResult downloadToFile(const FileRequest& request, const ProgressFn& progress) = 0;
};

// WinHTTP on Windows (downloader_win.cpp, NOT VALIDATED: written without a Windows compiler).
// Elsewhere a stub whose every call fails with a clear message (the app is Windows-only).
std::unique_ptr<IHttpDownloader> makeDefaultDownloader();

}  // namespace hb::marketplace
