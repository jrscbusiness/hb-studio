// The single WinHTTP client of HB Studio. Windows only. NOT VALIDATED on real networks (verified by the
// Windows CI build). SECURITY: HTTPS certificate validation is WinHTTP's default and is never relaxed here;
// never add WINHTTP_OPTION_SECURITY_FLAGS / SECURITY_FLAG_IGNORE_* (tests/net_source_guard_test.cpp enforces it).
#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <sstream>
#include <vector>

#include "net/body_reader.hpp"
#include "net/http.hpp"
#include "net/url.hpp"

#pragma comment(lib, "winhttp.lib")

namespace hb::net {
namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
    return w;
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(static_cast<std::size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), &s[0], n, nullptr, nullptr);
    return s;
}

struct Handle {
    HINTERNET h = nullptr;
    Handle() = default;
    explicit Handle(HINTERNET x) : h(x) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() {
        if (h) ::WinHttpCloseHandle(h);
    }
};

void fail(HttpResponse& r, const char* stage) {
    const DWORD e = ::GetLastError();
    r.error = (e == ERROR_WINHTTP_TIMEOUT) ? ErrorKind::Timeout : ErrorKind::Transport;
    r.errorStage = stage;
    r.nativeError = e;
    switch (e) {
        case ERROR_WINHTTP_TIMEOUT: r.failure = Failure::Timeout; break;
        case ERROR_WINHTTP_SECURE_FAILURE:
        case ERROR_WINHTTP_SECURE_CERT_CN_INVALID:
        case ERROR_WINHTTP_SECURE_CERT_DATE_INVALID:
        case ERROR_WINHTTP_SECURE_INVALID_CA:
        case ERROR_WINHTTP_SECURE_INVALID_CERT:
        case ERROR_WINHTTP_SECURE_CERT_REV_FAILED: r.failure = Failure::Tls; break;
        case ERROR_WINHTTP_NAME_NOT_RESOLVED: r.failure = Failure::NameNotResolved; break;
        case ERROR_WINHTTP_CANNOT_CONNECT: r.failure = Failure::CannotConnect; break;
        case ERROR_WINHTTP_CONNECTION_ERROR: r.failure = Failure::ConnectionError; break;
        case ERROR_WINHTTP_REDIRECT_FAILED: r.failure = Failure::RedirectFailed; break;
        default: r.failure = Failure::Other; break;
    }
    r.errorText = std::string(stage) + " failed (WinHTTP " + std::to_string(static_cast<unsigned long>(e)) + ")";
}

void parseRawHeaders(HINTERNET request, Headers& out) {
    DWORD hsize = 0;
    ::WinHttpQueryHeaders(request, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER, &hsize,
                          WINHTTP_NO_HEADER_INDEX);
    if (hsize == 0 || hsize >= 256 * 1024) return;
    std::wstring raw(hsize / sizeof(wchar_t), L'\0');
    if (!::WinHttpQueryHeaders(request, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, &raw[0], &hsize,
                               WINHTTP_NO_HEADER_INDEX))
        return;
    std::stringstream ss(narrow(raw.c_str()));
    std::string line;
    bool first = true;
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (first) { first = false; continue; }  // status line
        const auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string v = line.substr(colon + 1);
        while (!v.empty() && v.front() == ' ') v.erase(0, 1);
        out.emplace_back(line.substr(0, colon), v);
    }
}

class WinHttpClient : public IHttpClient {
public:
    explicit WinHttpClient(const HttpClientOptions& o) {
        const std::wstring ua = widen(o.userAgent);
        DWORD access = WINHTTP_ACCESS_TYPE_DEFAULT_PROXY;
        if (o.proxy == ProxyMode::Automatic) access = WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY;
        else if (o.proxy == ProxyMode::None) access = WINHTTP_ACCESS_TYPE_NO_PROXY;
        session_.h = ::WinHttpOpen(ua.c_str(), access, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session_.h && o.proxy == ProxyMode::Automatic)
            session_.h = ::WinHttpOpen(ua.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (session_.h && o.secureProtocols != 0) {
            DWORD protocols = static_cast<DWORD>(o.secureProtocols);
            ::WinHttpSetOption(session_.h, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof protocols);
        }
    }

    HttpResponse send(const HttpRequest& req) override {
        HttpResponse resp;
        try {
            sendImpl(req, resp);
        } catch (...) {
            resp = HttpResponse{};
            resp.error = ErrorKind::Transport;
            resp.errorText = "unexpected error in the HTTP transport";
        }
        return resp;
    }

private:
    void sendImpl(const HttpRequest& req, HttpResponse& resp) {
        Url url;
        resp.error = validateRequest(req, url, resp.errorText);
        if (resp.error != ErrorKind::None) return;
        if (!session_.h) {
            resp.error = ErrorKind::Transport;
            resp.errorStage = "WinHttpOpen";
            resp.errorText = "WinHttpOpen failed";
            return;
        }

        Handle conn(::WinHttpConnect(session_.h, widen(url.host).c_str(), static_cast<INTERNET_PORT>(url.port), 0));
        if (!conn.h) { fail(resp, "WinHttpConnect"); return; }
        Handle request(::WinHttpOpenRequest(conn.h, widen(req.method).c_str(), widen(url.target).c_str(), nullptr, WINHTTP_NO_REFERER,
                                            WINHTTP_DEFAULT_ACCEPT_TYPES, url.https() ? WINHTTP_FLAG_SECURE : 0));
        if (!request.h) { fail(resp, "WinHttpOpenRequest"); return; }

        DWORD policy = req.redirects == RedirectPolicy::Never ? WINHTTP_OPTION_REDIRECT_POLICY_NEVER
                                                              : WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
        ::WinHttpSetOption(request.h, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof policy);
        if (req.maxRedirects > 0) {
            DWORD maxRedirects = static_cast<DWORD>(req.maxRedirects);
            ::WinHttpSetOption(request.h, WINHTTP_OPTION_MAX_HTTP_AUTOMATIC_REDIRECTS, &maxRedirects, sizeof maxRedirects);
        }
        // Per request: the session may be shared by several threads.
        ::WinHttpSetTimeouts(request.h, req.timeouts.resolveMs, req.timeouts.connectMs, req.timeouts.sendMs, req.timeouts.receiveMs);

        std::wstring headers;
        for (const auto& h : req.headers) headers += widen(h.first) + L": " + widen(h.second) + L"\r\n";
        LPCWSTR hdrPtr = headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str();
        const DWORD hdrLen = headers.empty() ? 0 : static_cast<DWORD>(-1L);

        if (req.bodySource) {
            const DWORD total = static_cast<DWORD>(req.bodySourceLength);
            if (!::WinHttpSendRequest(request.h, hdrPtr, hdrLen, WINHTTP_NO_REQUEST_DATA, 0, total, 0)) { fail(resp, "WinHttpSendRequest"); return; }
            std::vector<char> buf(std::max<std::size_t>(req.bodySourceChunk, 1));
            std::uint64_t sent = 0;
            while (sent < req.bodySourceLength) {
                const std::size_t want = static_cast<std::size_t>(std::min<std::uint64_t>(buf.size(), req.bodySourceLength - sent));
                const std::size_t n = req.bodySource(buf.data(), want);
                if (n == kBodyAbort) {
                    resp.error = ErrorKind::Cancelled;
                    resp.errorText = "upload aborted by the body source";
                    return;
                }
                if (n == 0 || n > want) {
                    resp.error = ErrorKind::Transport;
                    resp.errorText = "body source ended early";
                    return;
                }
                DWORD written = 0;
                if (!::WinHttpWriteData(request.h, buf.data(), static_cast<DWORD>(n), &written)) { fail(resp, "WinHttpWriteData"); return; }
                sent += written ? written : n;
            }
        } else {
            const DWORD bodyLen = static_cast<DWORD>(req.body.size());
            if (!::WinHttpSendRequest(request.h, hdrPtr, hdrLen, req.body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(req.body.data()),
                                      bodyLen, bodyLen, 0)) {
                fail(resp, "WinHttpSendRequest");
                return;
            }
        }
        if (!::WinHttpReceiveResponse(request.h, nullptr)) { fail(resp, "WinHttpReceiveResponse"); return; }

        DWORD status = 0, size = sizeof status;
        ::WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                              WINHTTP_NO_HEADER_INDEX);
        resp.status = static_cast<int>(status);
        parseRawHeaders(request.h, resp.headers);
        {
            const std::string cl = resp.header("Content-Length");
            if (!cl.empty()) resp.contentLength = std::strtoull(cl.c_str(), nullptr, 10);
        }

        BodyReader reader(req, resp);
        if (!reader.wantsBody()) return;
        const bool hasDeadline = req.bufferedBodyDeadline.count() > 0 && !reader.streaming();
        const auto deadline = std::chrono::steady_clock::now() + req.bufferedBodyDeadline;
        std::vector<char> buf(64 * 1024);
        for (;;) {
            if (hasDeadline && std::chrono::steady_clock::now() > deadline) {
                resp.error = ErrorKind::Timeout;
                resp.errorText = "timed out reading the response";
                return;
            }
            DWORD avail = 0;
            if (!::WinHttpQueryDataAvailable(request.h, &avail)) { fail(resp, "WinHttpQueryDataAvailable"); return; }
            if (avail == 0) break;  // end of body
            if (avail > buf.size()) avail = static_cast<DWORD>(buf.size());
            DWORD got = 0;
            if (!::WinHttpReadData(request.h, buf.data(), avail, &got)) { fail(resp, "WinHttpReadData"); return; }
            if (got == 0) break;
            if (!reader.push(buf.data(), got)) return;
        }
    }

    Handle session_;
};

}  // namespace

std::shared_ptr<IHttpClient> makeWinHttpClient(const HttpClientOptions& options) { return std::make_shared<WinHttpClient>(options); }

}  // namespace hb::net

#endif  // _WIN32
