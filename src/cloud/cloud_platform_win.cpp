// Windows platform pieces of the cloud module. NOT VALIDATED: written without a Windows compiler;
// Windows CI / a manual test on Windows 11 is the real verifier (see docs/BACKUP_NUVEM.md).
#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#include <wincrypt.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>

#include "cloud/cloud_platform.hpp"
#include "cloud/hash_util.hpp"
#include "core/json.hpp"

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "crypt32.lib")

namespace hb::cloud {
namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string narrowStr(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<std::size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
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
    return makeError(ErrorKind::Transient, std::string("Falha de rede (") + what + ", erro WinHTTP " + std::to_string(e) + ")");
}

bool iequals(const std::string& a, const char* b) {
    const std::size_t n = std::char_traits<char>::length(b);
    if (a.size() != n) return false;
    for (std::size_t i = 0; i < n; ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}

class WinHttpCloud : public ICloudHttp {
public:
    WinHttpCloud() {
        session_.h = ::WinHttpOpen(L"HBStudio-Cloud/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                   WINHTTP_NO_PROXY_BYPASS, 0);
    }

    Result<HttpResponse> send(const HttpRequest& req) override {
        if (!session_.h) return makeError(ErrorKind::Transient, "WinHttpOpen falhou");
        const std::wstring wurl = widen(req.url);
        URL_COMPONENTS uc{};
        uc.dwStructSize = sizeof uc;
        uc.dwSchemeLength = uc.dwHostNameLength = uc.dwUrlPathLength = uc.dwExtraInfoLength = static_cast<DWORD>(-1);
        if (!::WinHttpCrackUrl(wurl.c_str(), static_cast<DWORD>(wurl.size()), 0, &uc)) return makeError(ErrorKind::Permanent, "URL inválida");
        const bool https = uc.nScheme == INTERNET_SCHEME_HTTPS;
        if (!https && uc.nScheme != INTERNET_SCHEME_HTTP) return makeError(ErrorKind::Permanent, "Esquema de URL não suportado");
        const std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
        std::wstring target(uc.lpszUrlPath, uc.dwUrlPathLength);
        if (uc.dwExtraInfoLength) target.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);
        if (target.empty()) target = L"/";

        Handle conn(::WinHttpConnect(session_.h, host.c_str(), uc.nPort, 0));
        if (!conn.h) return winError("connect");
        Handle request(::WinHttpOpenRequest(conn.h, widen(req.method).c_str(), target.c_str(), nullptr, WINHTTP_NO_REFERER,
                                            WINHTTP_DEFAULT_ACCEPT_TYPES, https ? WINHTTP_FLAG_SECURE : 0));
        if (!request.h) return winError("open request");
        // Never follow redirects: Drive's 308 "resume incomplete" must reach us as-is and a wrong
        // S3 region must surface as an error instead of silently re-sending a multi-MiB body.
        DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        ::WinHttpSetOption(request.h, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof policy);
        const int t = static_cast<int>(std::min<long long>(req.timeout.count(), 1800000));
        ::WinHttpSetTimeouts(request.h, 15000, 30000, 60000, t);

        std::wstring headers;
        for (const auto& h : req.headers) {
            if (iequals(h.first, "host") || iequals(h.first, "content-length")) continue;
            headers += widen(h.first) + L": " + widen(h.second) + L"\r\n";
        }
        const DWORD total = static_cast<DWORD>(req.body.size());
        if (req.body.size() > 0xFFFFFFF0ull) return makeError(ErrorKind::Permanent, "Corpo da requisição grande demais");
        if (!::WinHttpSendRequest(request.h, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                                  headers.empty() ? 0 : static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, total, 0))
            return winError("send");
        // Body in slices, each one cleared by the gate first (bandwidth cap / pause / cancel).
        constexpr std::size_t kSlice = 32 * 1024;
        for (std::size_t off = 0; off < req.body.size();) {
            const std::size_t n = std::min(kSlice, req.body.size() - off);
            if (req.gate && !req.gate->acquire(n)) return makeError(ErrorKind::Cancelled, "Envio interrompido");
            DWORD written = 0;
            if (!::WinHttpWriteData(request.h, req.body.data() + off, static_cast<DWORD>(n), &written)) return winError("write");
            off += written ? written : n;
        }
        if (!::WinHttpReceiveResponse(request.h, nullptr)) return winError("receive");

        HttpResponse resp;
        DWORD status = 0, size = sizeof status;
        ::WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                              WINHTTP_NO_HEADER_INDEX);
        resp.status = static_cast<int>(status);
        // Raw headers -> name/value pairs.
        DWORD hsize = 0;
        ::WinHttpQueryHeaders(request.h, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER, &hsize,
                              WINHTTP_NO_HEADER_INDEX);
        if (hsize > 0 && hsize < 256 * 1024) {
            std::wstring raw(hsize / sizeof(wchar_t), L'\0');
            if (::WinHttpQueryHeaders(request.h, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, raw.data(), &hsize,
                                      WINHTTP_NO_HEADER_INDEX)) {
                std::stringstream ss(narrowStr(raw.c_str()));
                std::string line;
                bool firstLine = true;
                while (std::getline(ss, line)) {
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    if (firstLine) { firstLine = false; continue; }  // status line
                    const auto colon = line.find(':');
                    if (colon == std::string::npos) continue;
                    std::string v = line.substr(colon + 1);
                    while (!v.empty() && v.front() == ' ') v.erase(0, 1);
                    resp.headers.emplace_back(line.substr(0, colon), v);
                }
            }
        }
        for (;;) {
            DWORD avail = 0;
            if (!::WinHttpQueryDataAvailable(request.h, &avail)) return winError("query data");
            if (avail == 0) break;
            std::string chunk(avail, '\0');
            DWORD read = 0;
            if (!::WinHttpReadData(request.h, chunk.data(), avail, &read)) return winError("read");
            if (resp.body.size() + read <= 4 * 1024 * 1024) resp.body.append(chunk.data(), read);  // cap: ignore the excess
        }
        return resp;
    }

private:
    Handle session_;
};

// ---- DPAPI ----
class DpapiStore : public ISecretStore {
public:
    explicit DpapiStore(std::string path) : path_(std::move(path)) { load(); }

    bool put(const std::string& name, const std::string& value) override {
        std::lock_guard lock(mu_);
        DATA_BLOB in{static_cast<DWORD>(value.size()), reinterpret_cast<BYTE*>(const_cast<char*>(value.data()))};
        DATA_BLOB entropy = entropyBlob();
        DATA_BLOB out{};
        if (!::CryptProtectData(&in, L"HBStudio cloud", &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return false;
        items_[name] = base64Encode(std::string_view(reinterpret_cast<const char*>(out.pbData), out.cbData));
        ::LocalFree(out.pbData);
        return save();
    }

    std::optional<std::string> get(const std::string& name) override {
        std::lock_guard lock(mu_);
        auto it = items_.find(name);
        if (it == items_.end()) return std::nullopt;
        std::string blob = base64Decode(it->second);
        if (blob.empty()) return std::nullopt;
        DATA_BLOB in{static_cast<DWORD>(blob.size()), reinterpret_cast<BYTE*>(blob.data())};
        DATA_BLOB entropy = entropyBlob();
        DATA_BLOB out{};
        if (!::CryptUnprotectData(&in, nullptr, &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return std::nullopt;
        std::string v(reinterpret_cast<const char*>(out.pbData), out.cbData);
        ::SecureZeroMemory(out.pbData, out.cbData);
        ::LocalFree(out.pbData);
        return v;
    }

    bool erase(const std::string& name) override {
        std::lock_guard lock(mu_);
        if (items_.erase(name) == 0) return false;
        return save();
    }

private:
    static DATA_BLOB entropyBlob() {
        static const char kE[] = "HBStudio.cloud.v1";
        return DATA_BLOB{static_cast<DWORD>(sizeof kE - 1), reinterpret_cast<BYTE*>(const_cast<char*>(kE))};
    }
    std::filesystem::path p() const { return std::filesystem::path(std::u8string(path_.begin(), path_.end())); }
    void load() {
        std::string text;
        {
            std::ifstream f(p(), std::ios::binary);
            if (!f) return;
            std::stringstream ss;
            ss << f.rdbuf();
            text = ss.str();
        }
        hb::core::json::Value r;
        if (!hb::core::json::parse(text, r) || !r.isObj()) return;
        const auto* items = r.get("items");
        if (!items || !items->isObj()) return;
        for (const auto& kv : items->o)
            if (kv.second.isStr()) items_[kv.first] = kv.second.s;
    }
    bool save() {
        hb::core::json::Value root, items;
        root.t = hb::core::json::Value::T::Obj;
        items.t = hb::core::json::Value::T::Obj;
        for (const auto& kv : items_) {
            hb::core::json::Value v;
            v.t = hb::core::json::Value::T::Str;
            v.s = kv.second;
            items.o[kv.first] = v;
        }
        hb::core::json::Value ver;
        ver.t = hb::core::json::Value::T::Num;
        ver.n = 1;
        root.o["v"] = ver;
        root.o["items"] = items;
        const std::string tmp = path_ + ".tmp";
        const auto tp = std::filesystem::path(std::u8string(tmp.begin(), tmp.end()));
        {
            std::ofstream f(tp, std::ios::binary | std::ios::trunc);
            if (!f) return false;
            f << hb::core::json::dump(root, true);
            if (!f.good()) return false;
        }
        std::error_code ec;
        std::filesystem::rename(tp, p(), ec);
        return !ec;
    }
    std::mutex mu_;
    std::string path_;
    std::map<std::string, std::string> items_;
};

// std::filesystem + "is another process writing this file?" probe: opening with no sharing fails
// with a sharing violation while a recorder still holds the file open.
class WinFs : public IFileSystem {
public:
    WinFs() : base_(makeStdFileSystem()) {}
    FileStat stat(const std::string& p) override { return base_->stat(p); }
    std::vector<std::string> listFiles(const std::string& d) override { return base_->listFiles(d); }
    bool read(const std::string& p, std::uint64_t o, std::size_t l, std::string& out) override { return base_->read(p, o, l, out); }
    bool remove(const std::string& p) override { return base_->remove(p); }
    bool isLocked(const std::string& p) override {
        HANDLE h = ::CreateFileW(widen(p).c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return ::GetLastError() == ERROR_SHARING_VIOLATION;
        ::CloseHandle(h);
        return false;
    }

private:
    std::shared_ptr<IFileSystem> base_;
};

}  // namespace

std::shared_ptr<ICloudHttp> makePlatformHttp() { return std::make_shared<WinHttpCloud>(); }
std::shared_ptr<ISecretStore> makePlatformSecretStore(const std::string& path) { return std::make_shared<DpapiStore>(path); }
std::shared_ptr<IFileSystem> makePlatformFileSystem() { return std::make_shared<WinFs>(); }

void setBackgroundThreadPriority() {
    // BACKGROUND_BEGIN lowers CPU, I/O and memory priority together: the live pipeline always wins.
    if (!::SetThreadPriority(::GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN)) ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_LOWEST);
}

}  // namespace hb::cloud
#endif
