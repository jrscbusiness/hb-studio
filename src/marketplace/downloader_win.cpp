// WinHTTP downloader for the plugin marketplace. NOT VALIDATED: written without access to a Windows
// compiler. Build and test on Windows 11 / MSVC 2022 before relying on it (see docs/MARKETPLACE.md).
//
// Security properties (by construction, review these first):
//   * only https:// URLs that pass isSafeHttpsUrl() are accepted;
//   * WINHTTP_FLAG_SECURE + no WINHTTP_OPTION_SECURITY_FLAGS override: certificate and host name
//     verification stay on (there is deliberately no switch to turn them off);
//   * TLS 1.2 / 1.3 only; redirects capped and never allowed to downgrade from https to http;
//   * the size cap is enforced while reading, not after;
//   * the file is written to "<dest>.part" and renamed only when complete.
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
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "marketplace/catalog.hpp"
#include "marketplace/downloader.hpp"

#pragma comment(lib, "winhttp.lib")

namespace hb::marketplace {
namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

fs::path pathFromUtf8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

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
    std::wstring host, path;
    INTERNET_PORT port = 443;
};

// Precondition: isSafeHttpsUrl(url) (so the authority has no userinfo / brackets and a valid port).
ParsedUrl parseSafeUrl(const std::string& url) {
    ParsedUrl p;
    std::string rest = url.substr(8);                       // after "https://"
    const std::size_t hash = rest.find('#');
    if (hash != std::string::npos) rest.resize(hash);
    const std::size_t end = rest.find_first_of("/?");
    std::string authority = rest.substr(0, end);
    std::string path = end == std::string::npos ? "/" : rest.substr(end);
    if (path.front() == '?') path = "/" + path;
    const std::size_t colon = authority.find(':');
    if (colon != std::string::npos) {
        p.port = static_cast<INTERNET_PORT>(std::stoi(authority.substr(colon + 1)));
        authority.resize(colon);
    }
    p.host = widen(authority);
    p.path = widen(path);
    return p;
}

std::string winErrorText(DWORD e) {
    switch (e) {
        case ERROR_WINHTTP_TIMEOUT: return "tempo esgotado";
        case ERROR_WINHTTP_SECURE_FAILURE:
        case ERROR_WINHTTP_SECURE_CERT_CN_INVALID:
        case ERROR_WINHTTP_SECURE_CERT_DATE_INVALID:
        case ERROR_WINHTTP_SECURE_INVALID_CA:
        case ERROR_WINHTTP_SECURE_INVALID_CERT:
        case ERROR_WINHTTP_SECURE_CERT_REV_FAILED:
            return "falha na verificação do certificado HTTPS";
        case ERROR_WINHTTP_NAME_NOT_RESOLVED: return "servidor não encontrado (sem internet?)";
        case ERROR_WINHTTP_CANNOT_CONNECT: return "não foi possível conectar ao servidor";
        case ERROR_WINHTTP_CONNECTION_ERROR: return "conexão interrompida";
        case ERROR_WINHTTP_REDIRECT_FAILED: return "redirecionamento recusado";
        default: return "erro de rede " + std::to_string(e);
    }
}

// One HTTPS GET: opens the request, sends it and receives the response headers.
class Exchange {
public:
    explicit Exchange(HINTERNET session) : session_(session) {}

    // Returns an empty string on success, else a pt-BR error.
    std::string open(const std::string& url, const std::wstring& extraHeaders, std::chrono::milliseconds timeout) {
        if (!isSafeHttpsUrl(url)) return "endereço de download não permitido (somente https)";
        const ParsedUrl u = parseSafeUrl(url);
        const int t = static_cast<int>(std::min<long long>(timeout.count(), 600000));
        ::WinHttpSetTimeouts(session_, t, t, t, t);
        conn_.h = ::WinHttpConnect(session_, u.host.c_str(), u.port, 0);
        if (!conn_.h) return winErrorText(::GetLastError());
        req_.h = ::WinHttpOpenRequest(conn_.h, L"GET", u.path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (!req_.h) return winErrorText(::GetLastError());
        DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
        ::WinHttpSetOption(req_.h, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof policy);
        DWORD maxRedirects = 5;
        ::WinHttpSetOption(req_.h, WINHTTP_OPTION_MAX_HTTP_AUTOMATIC_REDIRECTS, &maxRedirects, sizeof maxRedirects);
        if (!::WinHttpSendRequest(req_.h, extraHeaders.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : extraHeaders.c_str(),
                                  extraHeaders.empty() ? 0 : static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0))
            return winErrorText(::GetLastError());
        if (!::WinHttpReceiveResponse(req_.h, nullptr)) return winErrorText(::GetLastError());
        DWORD status = 0, size = sizeof status;
        if (!::WinHttpQueryHeaders(req_.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                                   WINHTTP_NO_HEADER_INDEX))
            return winErrorText(::GetLastError());
        status_ = static_cast<int>(status);
        return {};
    }

    int status() const { return status_; }

    std::uint64_t contentLength() const {
        DWORD len = 0, size = sizeof len;
        if (!::WinHttpQueryHeaders(req_.h, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &len, &size,
                                   WINHTTP_NO_HEADER_INDEX))
            return 0;
        return len;
    }

    std::string etag() const {
        wchar_t buf[256] = {0};
        DWORD size = sizeof buf - sizeof(wchar_t);
        if (!::WinHttpQueryHeaders(req_.h, WINHTTP_QUERY_ETAG, WINHTTP_HEADER_NAME_BY_INDEX, buf, &size, WINHTTP_NO_HEADER_INDEX)) return {};
        std::string out;
        for (DWORD i = 0; i < size / sizeof(wchar_t) && buf[i]; ++i)
            if (buf[i] >= 0x20 && buf[i] < 0x7F) out += static_cast<char>(buf[i]);   // printable ASCII only
        return out;
    }

    // Reads the next chunk; returns false on error (errOut set); `got` == 0 with true means end of body.
    bool read(std::vector<char>& buf, std::size_t& got, std::string& errOut) {
        got = 0;
        DWORD avail = 0;
        if (!::WinHttpQueryDataAvailable(req_.h, &avail)) {
            errOut = winErrorText(::GetLastError());
            return false;
        }
        if (avail == 0) return true;
        if (avail > buf.size()) avail = static_cast<DWORD>(buf.size());
        DWORD n = 0;
        if (!::WinHttpReadData(req_.h, buf.data(), avail, &n)) {
            errOut = winErrorText(::GetLastError());
            return false;
        }
        got = n;
        return true;
    }

private:
    HINTERNET session_;
    Handle conn_, req_;
    int status_ = 0;
};

class WinHttpDownloader : public IHttpDownloader {
public:
    WinHttpDownloader() {
        session_.h = ::WinHttpOpen(L"HBStudio-Marketplace/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (session_.h) {
            DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
            protocols |= WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
#endif
            ::WinHttpSetOption(session_.h, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof protocols);
        }
    }

    FetchResponse fetch(const FetchRequest& rq) override {
        FetchResponse out;
        if (!session_.h) { out.error = "não foi possível iniciar o WinHTTP"; return out; }
        std::wstring headers = L"Accept: application/json\r\n";
        if (!rq.ifNoneMatch.empty()) {
            std::string tag;
            for (unsigned char c : rq.ifNoneMatch)
                if (c >= 0x20 && c < 0x7F) tag += static_cast<char>(c);   // no CR/LF injection
            headers += L"If-None-Match: " + widen(tag) + L"\r\n";
        }
        const auto deadline = Clock::now() + rq.timeout;
        Exchange ex(session_.h);
        const std::string err = ex.open(rq.url, headers, rq.timeout);
        if (!err.empty()) { out.error = err; return out; }
        out.transportOk = true;
        out.status = ex.status();
        out.etag = ex.etag();
        if (out.status != 200) return out;   // 304 / errors carry no body we care about
        if (ex.contentLength() > rq.maxBytes) {
            out.transportOk = false;
            out.error = "resposta grande demais";
            return out;
        }
        std::vector<char> buf(16 * 1024);
        for (;;) {
            if (Clock::now() > deadline) { out = {}; out.error = "tempo esgotado"; return out; }
            std::size_t got = 0;
            std::string e;
            if (!ex.read(buf, got, e)) { out = {}; out.error = e; return out; }
            if (got == 0) break;
            if (out.body.size() + got > rq.maxBytes) { out = {}; out.error = "resposta grande demais"; return out; }
            out.body.append(buf.data(), got);
        }
        return out;
    }

    FileResult downloadToFile(const FileRequest& rq, const ProgressFn& progress) override {
        FileResult out;
        if (!session_.h) { out.error = "não foi possível iniciar o WinHTTP"; return out; }
        if (rq.maxBytes == 0 || rq.destPathUtf8.empty()) { out.error = "pedido de download inválido"; return out; }
        const auto deadline = Clock::now() + rq.timeout;
        Exchange ex(session_.h);
        const std::string err = ex.open(rq.url, L"Accept: application/octet-stream, */*\r\n", rq.timeout);
        if (!err.empty()) { out.error = err; return out; }
        out.status = ex.status();
        if (out.status != 200) { out.error = "o servidor respondeu HTTP " + std::to_string(out.status); return out; }
        const std::uint64_t total = ex.contentLength();
        if (total > rq.maxBytes) { out.error = "o arquivo é maior que o tamanho declarado no catálogo"; return out; }

        const fs::path dest = pathFromUtf8(rq.destPathUtf8);
        fs::path part = dest;
        part += ".part";
        std::error_code ec;
        {
            std::ofstream f(part, std::ios::binary | std::ios::trunc);
            if (!f) { out.error = "não foi possível gravar o arquivo temporário"; return out; }
            std::vector<char> buf(64 * 1024);
            std::uint64_t received = 0;
            bool failed = false;
            for (;;) {
                if (Clock::now() > deadline) { out.error = "tempo esgotado"; failed = true; break; }
                std::size_t got = 0;
                std::string e;
                if (!ex.read(buf, got, e)) { out.error = e; failed = true; break; }
                if (got == 0) break;
                received += got;
                if (received > rq.maxBytes) { out.error = "o arquivo é maior que o tamanho declarado no catálogo"; failed = true; break; }
                f.write(buf.data(), static_cast<std::streamsize>(got));
                if (!f) { out.error = "falha ao gravar o arquivo (disco cheio?)"; failed = true; break; }
                if (progress && !progress(received, total ? total : rq.maxBytes)) { out.cancelled = true; out.error = "cancelado"; failed = true; break; }
            }
            f.close();
            if (!failed && !f) { out.error = "falha ao gravar o arquivo"; failed = true; }
            if (failed) { fs::remove(part, ec); return out; }
            out.bytes = received;
        }
        fs::remove(dest, ec);
        fs::rename(part, dest, ec);
        if (ec) { fs::remove(part, ec); out.error = "não foi possível finalizar o arquivo"; out.bytes = 0; return out; }
        out.ok = true;
        return out;
    }

private:
    Handle session_;
};

}  // namespace

std::unique_ptr<IHttpDownloader> makeDefaultDownloader() { return std::make_unique<WinHttpDownloader>(); }

}  // namespace hb::marketplace

#endif  // _WIN32
