// WinHTTP client. NOT VALIDATED: written without access to a Windows compiler. Build and test on
// Windows 11 / MSVC 2022 before relying on it (see docs/ONVIF.md).
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

#include "onvif/http_client.hpp"
#include "onvif/url.hpp"

#pragma comment(lib, "winhttp.lib")

namespace hb::onvif {
namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

struct Handle {
    HINTERNET h = nullptr;
    Handle() = default;
    explicit Handle(HINTERNET x) : h(x) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() { if (h) ::WinHttpCloseHandle(h); }
};

Error winError(const char* what) {
    const DWORD e = ::GetLastError();
    Error err = makeError(e == ERROR_WINHTTP_TIMEOUT ? ErrorCode::Timeout : ErrorCode::Transport,
                          std::string(what) + " failed (WinHTTP error " + std::to_string(e) + ")");
    return err;
}

class WinHttpClient : public IHttpClient {
public:
    WinHttpClient() { session_.h = ::WinHttpOpen(L"HBStudio-ONVIF/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0); }

    Result<HttpResponse> send(const HttpRequest& req) override {
        if (!session_.h) return makeError(ErrorCode::Transport, "WinHttpOpen failed");
        auto parsed = parseUrl(req.url);
        if (!parsed.ok()) return parsed.error();
        const Url& url = parsed.value();
        if (url.scheme != "http" && url.scheme != "https") return makeError(ErrorCode::InvalidArgument, "unsupported scheme");
        const int t = static_cast<int>(std::min<long long>(req.timeout.count(), 600000));
        ::WinHttpSetTimeouts(session_.h, t, t, t, t);

        Handle conn(::WinHttpConnect(session_.h, widen(url.host).c_str(), static_cast<INTERNET_PORT>(url.port), 0));
        if (!conn.h) return winError("WinHttpConnect");
        Handle request(::WinHttpOpenRequest(conn.h, widen(req.method).c_str(), widen(url.path).c_str(), nullptr, WINHTTP_NO_REFERER,
                                            WINHTTP_DEFAULT_ACCEPT_TYPES, url.scheme == "https" ? WINHTTP_FLAG_SECURE : 0));
        if (!request.h) return winError("WinHttpOpenRequest");

        std::wstring headers;
        for (const auto& h : req.headers) headers += widen(h.first) + L": " + widen(h.second) + L"\r\n";
        const BOOL sent = ::WinHttpSendRequest(request.h, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                                               headers.empty() ? 0 : static_cast<DWORD>(-1L),
                                               req.body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(req.body.data()),
                                               static_cast<DWORD>(req.body.size()), static_cast<DWORD>(req.body.size()), 0);
        if (!sent) return winError("WinHttpSendRequest");
        if (!::WinHttpReceiveResponse(request.h, nullptr)) return winError("WinHttpReceiveResponse");

        HttpResponse resp;
        DWORD status = 0, size = sizeof status;
        ::WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                              WINHTTP_NO_HEADER_INDEX);
        resp.status = static_cast<int>(status);
        for (;;) {
            DWORD avail = 0;
            if (!::WinHttpQueryDataAvailable(request.h, &avail)) return winError("WinHttpQueryDataAvailable");
            if (avail == 0) break;
            std::string chunk(avail, '\0');
            DWORD read = 0;
            if (!::WinHttpReadData(request.h, chunk.data(), avail, &read)) return winError("WinHttpReadData");
            resp.body.append(chunk.data(), read);
            if (resp.body.size() > kMaxHttpResponseBytes) return makeError(ErrorCode::Transport, "response too large");
        }
        return resp;
    }

private:
    Handle session_;
};

}  // namespace

std::unique_ptr<IHttpClient> makeDefaultHttpClient() { return std::make_unique<WinHttpClient>(); }

}  // namespace hb::onvif

#endif  // _WIN32
