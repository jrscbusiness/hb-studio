#include "chroma/windows/winhttp_client.hpp"

#include <windows.h>
#include <winhttp.h>

#include <string>
#include <vector>

namespace hb::chroma {

namespace {

// Closes a WinHTTP handle on scope exit.
struct Handle {
    HINTERNET h = nullptr;
    Handle() = default;
    explicit Handle(HINTERNET x) : h(x) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() {
        if (h) WinHttpCloseHandle(h);
    }
};

std::wstring widenUtf8(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
    return w;
}

std::string errText(const char* what) {
    return std::string(what) + " (erro WinHTTP " + std::to_string(GetLastError()) + ")";
}

}  // namespace

HttpResult WinHttpClient::get(const std::string& url,
                              const std::function<bool(const std::uint8_t*, std::size_t, std::uint64_t)>& onData) {
    HttpResult r;
    const std::wstring wurl = widenUtf8(url);
    if (wurl.empty()) {
        r.error = "URL inválida";
        return r;
    }

    wchar_t host[256] = {0};
    wchar_t path[2048] = {0};
    wchar_t extra[2048] = {0};
    URL_COMPONENTS uc;
    ZeroMemory(&uc, sizeof uc);
    uc.dwStructSize = sizeof uc;
    uc.lpszHostName = host;
    uc.dwHostNameLength = static_cast<DWORD>(sizeof host / sizeof host[0]);
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = static_cast<DWORD>(sizeof path / sizeof path[0]);
    uc.lpszExtraInfo = extra;
    uc.dwExtraInfoLength = static_cast<DWORD>(sizeof extra / sizeof extra[0]);
    if (!WinHttpCrackUrl(wurl.c_str(), static_cast<DWORD>(wurl.size()), 0, &uc)) {
        r.error = errText("URL inválida");
        return r;
    }
    if (uc.nScheme != INTERNET_SCHEME_HTTPS) {   // never plain HTTP for a download we then execute
        r.error = "Somente HTTPS é permitido";
        return r;
    }

    Handle session(WinHttpOpen(L"HBStudio", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                               WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.h) {
        r.error = errText("Não foi possível iniciar a conexão");
        return r;
    }
    // resolve, connect, send, receive timeouts (ms): a dead connection must not hang forever.
    WinHttpSetTimeouts(session.h, 10000, 10000, 20000, 30000);

    Handle conn(WinHttpConnect(session.h, host, uc.nPort, 0));
    if (!conn.h) {
        r.error = errText("Não foi possível conectar ao servidor");
        return r;
    }
    const std::wstring target = std::wstring(path) + std::wstring(extra);
    Handle req(WinHttpOpenRequest(conn.h, L"GET", target.c_str(), nullptr, WINHTTP_NO_REFERER,
                                  WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
    if (!req.h) {
        r.error = errText("Não foi possível criar a requisição");
        return r;
    }
    DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;   // follow redirects, never downgrade
    WinHttpSetOption(req.h, WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof redirectPolicy);

    if (!WinHttpSendRequest(req.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(req.h, nullptr)) {
        r.error = errText("Falha de rede");
        return r;
    }

    DWORD status = 0;
    DWORD statusSize = sizeof status;
    if (!WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                             &status, &statusSize, WINHTTP_NO_HEADER_INDEX)) {
        r.error = errText("Resposta inválida");
        return r;
    }
    r.status = static_cast<int>(status);
    if (status != 200) {
        r.error = "HTTP " + std::to_string(status);
        return r;
    }

    std::uint64_t total = 0;
    {
        wchar_t lenBuf[32] = {0};
        DWORD lenSize = sizeof lenBuf;
        if (WinHttpQueryHeaders(req.h, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, lenBuf, &lenSize,
                                WINHTTP_NO_HEADER_INDEX))
            total = static_cast<std::uint64_t>(_wtoi64(lenBuf));
    }

    std::vector<std::uint8_t> buf(16 * 1024);
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req.h, &avail)) {
            r.error = errText("Conexão interrompida");
            return r;
        }
        if (avail == 0) break;   // end of body
        if (avail > buf.size()) avail = static_cast<DWORD>(buf.size());
        DWORD got = 0;
        if (!WinHttpReadData(req.h, buf.data(), avail, &got)) {
            r.error = errText("Conexão interrompida");
            return r;
        }
        if (got == 0) break;
        if (!onData(buf.data(), got, total)) {
            r.error = "Interrompido";
            return r;   // caller aborted (cancel / size cap): ok stays false
        }
    }
    r.ok = true;
    return r;
}

}  // namespace hb::chroma
