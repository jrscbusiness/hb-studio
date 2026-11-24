// WinHTTP transport for the AI client. Windows only. NOT VALIDATED: written without a Windows
// compiler; HTTPS certificate validation is WinHTTP's default and is never relaxed here.
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
#include <string>

#include "ai/http.hpp"

#pragma comment(lib, "winhttp.lib")

namespace hb::ai {
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

struct ParsedUrl {
    bool https = false;
    std::string host;
    int port = 0;
    std::string path = "/";
};

bool parseUrl(const std::string& url, ParsedUrl& out) {
    std::string rest;
    if (url.rfind("https://", 0) == 0) { out.https = true; rest = url.substr(8); }
    else if (url.rfind("http://", 0) == 0) { out.https = false; rest = url.substr(7); }
    else return false;
    const std::size_t slash = rest.find('/');
    std::string authority = rest.substr(0, slash);
    out.path = slash == std::string::npos ? "/" : rest.substr(slash);
    out.port = out.https ? 443 : 80;
    std::string portStr;
    if (!authority.empty() && authority[0] == '[') {
        const std::size_t close = authority.find(']');
        if (close == std::string::npos) return false;
        out.host = authority.substr(1, close - 1);
        if (close + 1 < authority.size() && authority[close + 1] == ':') portStr = authority.substr(close + 2);
    } else {
        const std::size_t colon = authority.rfind(':');
        out.host = authority.substr(0, colon);
        if (colon != std::string::npos) portStr = authority.substr(colon + 1);
    }
    if (!portStr.empty()) out.port = std::atoi(portStr.c_str());
    return !out.host.empty() && out.port > 0 && out.port <= 65535;
}

class WinHttpAiClient : public IAiHttp {
public:
    WinHttpAiClient() {
        session_.h = ::WinHttpOpen(L"HBStudio-AI/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                   WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session_.h)
            session_.h = ::WinHttpOpen(L"HBStudio-AI/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                       WINHTTP_NO_PROXY_BYPASS, 0);
    }

    HttpResponse send(const HttpRequest& req) override {
        HttpResponse resp;
        try {
            sendImpl(req, resp);
        } catch (...) {
            resp.status = 0;
            resp.transportError = "erro inesperado no transporte HTTP";
        }
        return resp;
    }

private:
    static void fail(HttpResponse& r, const char* what) {
        const DWORD e = ::GetLastError();
        r.status = 0;
        r.timedOut = (e == ERROR_WINHTTP_TIMEOUT);
        r.transportError = std::string(what) + " falhou (WinHTTP " + std::to_string(static_cast<unsigned long>(e)) + ")";
    }

    void sendImpl(const HttpRequest& req, HttpResponse& resp) {
        if (!session_.h) { resp.transportError = "WinHttpOpen falhou"; return; }
        ParsedUrl url;
        if (!parseUrl(req.url, url)) { resp.transportError = "endereço inválido"; return; }
        const int t = static_cast<int>(std::min<long long>(std::max<long long>(req.timeout.count(), 1000), 600000));

        Handle conn(::WinHttpConnect(session_.h, widen(url.host).c_str(), static_cast<INTERNET_PORT>(url.port), 0));
        if (!conn.h) { fail(resp, "WinHttpConnect"); return; }
        Handle request(::WinHttpOpenRequest(conn.h, widen(req.method).c_str(), widen(url.path).c_str(), nullptr, WINHTTP_NO_REFERER,
                                            WINHTTP_DEFAULT_ACCEPT_TYPES, url.https ? WINHTTP_FLAG_SECURE : 0));
        if (!request.h) { fail(resp, "WinHttpOpenRequest"); return; }
        ::WinHttpSetTimeouts(request.h, t, t, t, t);   // per request: the session may be shared by threads

        std::wstring headers;
        for (const auto& h : req.headers) headers += widen(h.first) + L": " + widen(h.second) + L"\r\n";
        const DWORD bodyLen = static_cast<DWORD>(req.body.size());
        const BOOL sent = ::WinHttpSendRequest(request.h, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                                               headers.empty() ? 0 : static_cast<DWORD>(-1L),
                                               req.body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(req.body.data()), bodyLen,
                                               bodyLen, 0);
        if (!sent) { fail(resp, "WinHttpSendRequest"); return; }
        if (!::WinHttpReceiveResponse(request.h, nullptr)) { fail(resp, "WinHttpReceiveResponse"); return; }

        DWORD status = 0, size = sizeof status;
        ::WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                              WINHTTP_NO_HEADER_INDEX);
        resp.status = static_cast<int>(status);

        wchar_t ra[64] = {0};
        DWORD raSize = sizeof ra;
        if (::WinHttpQueryHeaders(request.h, WINHTTP_QUERY_CUSTOM, L"Retry-After", ra, &raSize, WINHTTP_NO_HEADER_INDEX))
            resp.headers.emplace_back("Retry-After", narrow(ra));

        const bool streaming = static_cast<bool>(req.onData) && resp.status >= 200 && resp.status < 300;
        const auto deadline = std::chrono::steady_clock::now() + req.timeout;
        for (;;) {
            if (std::chrono::steady_clock::now() > deadline && !streaming) {
                resp.status = 0;
                resp.timedOut = true;
                resp.transportError = "tempo esgotado";
                return;
            }
            DWORD avail = 0;
            if (!::WinHttpQueryDataAvailable(request.h, &avail)) { fail(resp, "WinHttpQueryDataAvailable"); return; }
            if (avail == 0) break;
            std::string chunk(avail, '\0');
            DWORD read = 0;
            if (!::WinHttpReadData(request.h, &chunk[0], avail, &read)) { fail(resp, "WinHttpReadData"); return; }
            chunk.resize(read);
            if (streaming) {
                if (!req.onData(chunk)) break;  // consumer aborted
            } else {
                resp.body += chunk;
                if (resp.body.size() > kMaxAiResponseBytes) { resp.status = 0; resp.transportError = "resposta grande demais"; return; }
            }
        }
    }

    Handle session_;
};

}  // namespace

std::shared_ptr<IAiHttp> makeDefaultAiHttp() { return std::make_shared<WinHttpAiClient>(); }

}  // namespace hb::ai

#endif  // _WIN32
