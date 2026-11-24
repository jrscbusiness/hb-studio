// NOT VALIDATED: written without access to a Windows compiler; verify on Windows CI / real hardware.
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>

#include <filesystem>
#include <fstream>
#include <vector>

#include "captions/windows/winhttp_client.hpp"

#pragma comment(lib, "winhttp.lib")

namespace hb::captions::win {
namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
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

struct Target {
    std::wstring host, path;
    INTERNET_PORT port = 0;
    bool secure = false;
};

bool crack(const std::string& url, Target& t) {
    const std::wstring w = widen(url);
    if (w.empty()) return false;
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof uc;
    uc.dwHostNameLength = static_cast<DWORD>(-1);
    uc.dwUrlPathLength = static_cast<DWORD>(-1);
    uc.dwExtraInfoLength = static_cast<DWORD>(-1);
    uc.dwSchemeLength = static_cast<DWORD>(-1);
    if (!::WinHttpCrackUrl(w.c_str(), static_cast<DWORD>(w.size()), 0, &uc)) return false;
    if (uc.nScheme != INTERNET_SCHEME_HTTPS && uc.nScheme != INTERNET_SCHEME_HTTP) return false;
    t.host.assign(uc.lpszHostName, uc.dwHostNameLength);
    t.path.assign(uc.lpszUrlPath, uc.dwUrlPathLength);
    if (uc.dwExtraInfoLength) t.path.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);
    if (t.path.empty()) t.path = L"/";
    t.port = uc.nPort;
    t.secure = uc.nScheme == INTERNET_SCHEME_HTTPS;
    return true;
}

std::string lastErr(const char* what) {
    return std::string(what) + " (erro WinHTTP " + std::to_string(::GetLastError()) + ")";
}

}  // namespace

bool WinHttpClient::download(const std::string& url, const std::string& destPathUtf8,
                             const std::function<bool(std::uint64_t, std::uint64_t)>& progress, std::string& error) {
    Target t;
    if (!crack(url, t) || !t.secure) { error = "endereço de download inválido"; return false; }
    Handle session(::WinHttpOpen(L"HBStudio-Captions/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                 WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.h) { error = lastErr("WinHttpOpen"); return false; }
    ::WinHttpSetTimeouts(session.h, 15000, 15000, 30000, 60000);
    Handle conn(::WinHttpConnect(session.h, t.host.c_str(), t.port, 0));
    if (!conn.h) { error = lastErr("conexão"); return false; }
    Handle req(::WinHttpOpenRequest(conn.h, L"GET", t.path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                    WINHTTP_FLAG_SECURE));
    if (!req.h) { error = lastErr("requisição"); return false; }
    // Redirects (Hugging Face -> its CDN) are followed by WinHTTP; https->http downgrades are refused by default.
    if (!::WinHttpSendRequest(req.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !::WinHttpReceiveResponse(req.h, nullptr)) {
        error = lastErr("sem resposta do servidor");
        return false;
    }
    DWORD status = 0, sz = sizeof status;
    ::WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz,
                          WINHTTP_NO_HEADER_INDEX);
    if (status != 200) { error = "servidor respondeu HTTP " + std::to_string(status); return false; }
    DWORD len32 = 0;
    sz = sizeof len32;
    std::uint64_t total = 0;
    if (::WinHttpQueryHeaders(req.h, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &len32,
                              &sz, WINHTTP_NO_HEADER_INDEX))
        total = len32;

    std::ofstream out(std::filesystem::path(widen(destPathUtf8)), std::ios::binary | std::ios::trunc);
    if (!out) { error = "não foi possível criar o arquivo do modelo"; return false; }
    std::vector<char> buf(64 * 1024);
    std::uint64_t got = 0;
    for (;;) {
        DWORD n = 0;
        if (!::WinHttpReadData(req.h, buf.data(), static_cast<DWORD>(buf.size()), &n)) { error = lastErr("leitura interrompida"); return false; }
        if (n == 0) break;
        out.write(buf.data(), static_cast<std::streamsize>(n));
        if (!out) { error = "disco cheio ou sem permissão de escrita"; return false; }
        got += n;
        if (progress && !progress(got, total)) { error = "cancelado"; return false; }
    }
    out.close();
    if (total != 0 && got != total) { error = "download incompleto"; return false; }
    return true;
}

HttpResponse WinHttpClient::post(const std::string& url, const std::vector<std::pair<std::string, std::string>>& headers,
                                 const std::string& body, int timeoutMs) {
    HttpResponse resp;
    Target t;
    if (!crack(url, t)) { resp.error = "endereço inválido"; return resp; }
    Handle session(::WinHttpOpen(L"HBStudio-Captions/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                 WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.h) { resp.error = lastErr("WinHttpOpen"); return resp; }
    const int to = timeoutMs < 1000 ? 1000 : (timeoutMs > 120000 ? 120000 : timeoutMs);
    ::WinHttpSetTimeouts(session.h, to, to, to, to);
    Handle conn(::WinHttpConnect(session.h, t.host.c_str(), t.port, 0));
    if (!conn.h) { resp.error = lastErr("conexão"); return resp; }
    Handle req(::WinHttpOpenRequest(conn.h, L"POST", t.path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                    t.secure ? WINHTTP_FLAG_SECURE : 0));
    if (!req.h) { resp.error = lastErr("requisição"); return resp; }
    std::wstring hdr;
    for (const auto& h : headers) hdr += widen(h.first) + L": " + widen(h.second) + L"\r\n";
    const DWORD bodyLen = static_cast<DWORD>(body.size());
    if (!::WinHttpSendRequest(req.h, hdr.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : hdr.c_str(),
                              hdr.empty() ? 0 : static_cast<DWORD>(-1L), const_cast<char*>(body.data()), bodyLen, bodyLen, 0) ||
        !::WinHttpReceiveResponse(req.h, nullptr)) {
        resp.error = lastErr("sem resposta");
        return resp;
    }
    DWORD status = 0, sz = sizeof status;
    ::WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz,
                          WINHTTP_NO_HEADER_INDEX);
    resp.status = static_cast<int>(status);
    std::vector<char> buf(16 * 1024);
    for (;;) {
        DWORD n = 0;
        if (!::WinHttpReadData(req.h, buf.data(), static_cast<DWORD>(buf.size()), &n)) { resp.error = lastErr("leitura"); return resp; }
        if (n == 0) break;
        resp.body.append(buf.data(), n);
        if (resp.body.size() > (4u << 20)) break;   // a transcription reply is tiny; refuse anything huge
    }
    return resp;
}

}  // namespace hb::captions::win
#endif
