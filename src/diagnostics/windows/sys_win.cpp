#include "diagnostics/windows/sys_win.hpp"

#include <windows.h>
#include <dxgi.h>
#include <mfapi.h>
#include <mfidl.h>
#include <winhttp.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace hb::diag::sys {

namespace {

std::string narrowUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring widenUtf8(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string windowsVersionText() {
    using Fn = LONG(WINAPI*)(OSVERSIONINFOW*);
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    auto fn = nt ? reinterpret_cast<Fn>(GetProcAddress(nt, "RtlGetVersion")) : nullptr;
    if (!fn) return "Windows (versão desconhecida)";
    OSVERSIONINFOW vi{};
    vi.dwOSVersionInfoSize = sizeof(vi);
    if (fn(&vi) != 0) return "Windows (versão desconhecida)";
    char b[96];
    // "major.minor build N" rather than a dotted quad, so no later redaction pass mistakes it for an IP.
    std::snprintf(b, sizeof b, "Windows %lu.%lu build %lu%s", static_cast<unsigned long>(vi.dwMajorVersion),
                  static_cast<unsigned long>(vi.dwMinorVersion), static_cast<unsigned long>(vi.dwBuildNumber),
                  vi.dwBuildNumber >= 22000 ? " (Windows 11)" : "");
    return b;
}

std::string cpuNameText() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0, KEY_READ, &key) != ERROR_SUCCESS)
        return {};
    wchar_t buf[256] = {0};
    DWORD size = sizeof(buf) - sizeof(wchar_t), type = 0;
    std::string out;
    if (RegQueryValueExW(key, L"ProcessorNameString", nullptr, &type, reinterpret_cast<LPBYTE>(buf), &size) == ERROR_SUCCESS && type == REG_SZ)
        out = narrowUtf8(buf);
    RegCloseKey(key);
    return out;
}

std::vector<std::string> gpuNames() {
    std::vector<std::string> out;
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) || !factory) return out;
    for (UINT i = 0; i < 16; ++i) {
        IDXGIAdapter1* ad = nullptr;
        if (factory->EnumAdapters1(i, &ad) != S_OK || !ad) break;
        DXGI_ADAPTER_DESC1 d{};
        if (SUCCEEDED(ad->GetDesc1(&d)) && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            std::string s = narrowUtf8(d.Description);
            if (d.DedicatedVideoMemory > 0) s += " (" + std::to_string(static_cast<unsigned long long>(d.DedicatedVideoMemory / (1024 * 1024))) + " MB VRAM)";
            out.push_back(s);
        }
        ad->Release();
    }
    factory->Release();
    return out;
}

BOOL CALLBACK monitorCb(HMONITOR mon, HDC, LPRECT, LPARAM lp) {
    auto* v = reinterpret_cast<std::vector<std::string>*>(lp);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(mon, &mi)) {
        const long w = mi.rcMonitor.right - mi.rcMonitor.left, h = mi.rcMonitor.bottom - mi.rcMonitor.top;
        v->push_back(std::to_string(w) + "x" + std::to_string(h) + ((mi.dwFlags & MONITORINFOF_PRIMARY) ? " (principal)" : ""));
    }
    return TRUE;
}

// ------------------------------------------------------------------ probes

std::int64_t freeDisk(const std::string& folderUtf8) {
    std::wstring p = widenUtf8(folderUtf8);
    if (p.empty()) return -1;
    auto query = [](const std::wstring& path) -> std::int64_t {
        ULARGE_INTEGER avail{}, total{}, freeAll{};
        if (!GetDiskFreeSpaceExW(path.c_str(), &avail, &total, &freeAll)) return -1;
        return avail.QuadPart > static_cast<ULONGLONG>(INT64_MAX) ? INT64_MAX : static_cast<std::int64_t>(avail.QuadPart);
    };
    // Walk up to the nearest existing parent (the folder may not have been created yet).
    for (int guard = 0; guard < 64; ++guard) {
        const std::int64_t v = query(p);
        if (v >= 0) return v;
        while (!p.empty() && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
        const std::size_t cut = p.find_last_of(L"\\/");
        if (cut == std::wstring::npos) return -1;
        if (cut <= 2 && p.size() > 1 && p[1] == L':') return query(p.substr(0, 2) + L"\\");   // drive root
        p.resize(cut);
    }
    return -1;
}

struct HInternet {
    HINTERNET h = nullptr;
    explicit HInternet(HINTERNET x = nullptr) : h(x) {}
    ~HInternet() { if (h) WinHttpCloseHandle(h); }
    HInternet(const HInternet&) = delete;
    HInternet& operator=(const HInternet&) = delete;
};

std::int64_t fileTimeToEpoch(const FILETIME& ft) {
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    constexpr ULONGLONG kEpoch = 116444736000000000ULL;   // 1601 -> 1970 in 100 ns
    return u.QuadPart > kEpoch ? static_cast<std::int64_t>((u.QuadPart - kEpoch) / 10000000ULL) : 0;
}

NetProbeResult reachability() {
    NetProbeResult r;
    // The same host Windows itself uses for its "connected to the internet" indicator.
    HInternet session(WinHttpOpen(L"HBStudio-Checklist/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                  WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.h) { r.error = "WinHTTP indisponível"; return r; }
    WinHttpSetTimeouts(session.h, 4000, 4000, 4000, 4000);
    HInternet conn(WinHttpConnect(session.h, L"www.msftconnecttest.com", INTERNET_DEFAULT_HTTPS_PORT, 0));
    if (!conn.h) { r.error = "sem conexão"; return r; }
    HInternet req(WinHttpOpenRequest(conn.h, L"HEAD", L"/connecttest.txt", nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                     WINHTTP_FLAG_SECURE));
    if (!req.h) { r.error = "sem conexão"; return r; }
    if (!WinHttpSendRequest(req.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !WinHttpReceiveResponse(req.h, nullptr)) {
        char b[48];
        std::snprintf(b, sizeof b, "erro %lu", static_cast<unsigned long>(GetLastError()));
        r.error = b;
        return r;
    }
    r.reachable = true;
    SYSTEMTIME st{};
    DWORD sz = sizeof(st);
    if (WinHttpQueryHeaders(req.h, WINHTTP_QUERY_DATE | WINHTTP_QUERY_FLAG_SYSTEMTIME, WINHTTP_HEADER_NAME_BY_INDEX, &st, &sz,
                            WINHTTP_NO_HEADER_INDEX)) {
        FILETIME ft{};
        if (SystemTimeToFileTime(&st, &ft)) r.serverEpochSec = fileTimeToEpoch(ft);
    }
    return r;
}

UploadProbeResult uploadTest(const std::string& urlUtf8) {
    UploadProbeResult r;
    constexpr std::size_t kTotal = 512 * 1024;   // bounded payload
    constexpr std::size_t kChunk = 16 * 1024;
    constexpr DWORD kDeadlineMs = 10000;
    const std::wstring url = widenUtf8(urlUtf8);
    if (url.empty() || !isValidSpeedTestUrl(urlUtf8)) { r.error = "endereço inválido"; return r; }

    URL_COMPONENTSW uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {0}, path[1024] = {0};
    uc.lpszHostName = host;
    uc.dwHostNameLength = 255;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 1023;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc) || uc.nScheme != INTERNET_SCHEME_HTTPS) { r.error = "endereço inválido"; return r; }
    std::wstring objectName = std::wstring(uc.lpszUrlPath, uc.dwUrlPathLength);
    if (uc.lpszExtraInfo && uc.dwExtraInfoLength) objectName += std::wstring(uc.lpszExtraInfo, uc.dwExtraInfoLength);
    if (objectName.empty()) objectName = L"/";

    HInternet session(WinHttpOpen(L"HBStudio-Checklist/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                  WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.h) { r.error = "WinHTTP indisponível"; return r; }
    WinHttpSetTimeouts(session.h, 5000, 5000, 8000, 8000);
    HInternet conn(WinHttpConnect(session.h, std::wstring(uc.lpszHostName, uc.dwHostNameLength).c_str(), uc.nPort, 0));
    if (!conn.h) { r.error = "sem conexão"; return r; }
    HInternet req(WinHttpOpenRequest(conn.h, L"POST", objectName.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                     WINHTTP_FLAG_SECURE));
    if (!req.h) { r.error = "sem conexão"; return r; }
    const wchar_t* hdr = L"Content-Type: application/octet-stream\r\n";
    if (!WinHttpSendRequest(req.h, hdr, static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, static_cast<DWORD>(kTotal), 0)) {
        r.error = "não foi possível conectar";
        return r;
    }
    std::vector<unsigned char> chunk(kChunk);
    std::uint32_t x = 0x9E3779B9u;
    for (auto& b : chunk) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; b = static_cast<unsigned char>(x); }   // incompressible filler

    const auto t0 = std::chrono::steady_clock::now();
    std::size_t sent = 0;
    while (sent < kTotal) {
        const DWORD n = static_cast<DWORD>(std::min(kChunk, kTotal - sent));
        DWORD written = 0;
        if (!WinHttpWriteData(req.h, chunk.data(), n, &written) || written == 0) { r.error = "falha ao enviar"; return r; }
        sent += written;
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        if (ms > kDeadlineMs) break;
    }
    if (sent >= kTotal) {
        if (!WinHttpReceiveResponse(req.h, nullptr)) { r.error = "sem resposta"; return r; }
        DWORD status = 0, sz = sizeof(status);
        if (WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz,
                                WINHTTP_NO_HEADER_INDEX))
            r.httpStatus = static_cast<int>(status);
        if (r.httpStatus >= 400) { r.error = "HTTP " + std::to_string(r.httpStatus); return r; }
    } else if (sent < 64 * 1024) {
        r.error = "tempo esgotado";
        return r;
    }
    const auto ms = std::max<long long>(1, std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
    r.kbps = static_cast<double>(sent) * 8.0 / static_cast<double>(ms);   // bits per ms == kbit/s
    r.ok = true;
    return r;
}

EncoderProbeResult encoders() {
    EncoderProbeResult e;
    const HRESULT ci = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool comOwned = SUCCEEDED(ci);   // RPC_E_CHANGED_MODE: already initialised differently, fine
    const bool mf = SUCCEEDED(MFStartup(MF_VERSION));
    if (mf) {
        MFT_REGISTER_TYPE_INFO outInfo{MFMediaType_Video, MFVideoFormat_H264};
        auto enumerate = [&](UINT32 flags, bool& found) {
            IMFActivate** acts = nullptr;
            UINT32 n = 0;
            if (SUCCEEDED(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, flags, nullptr, &outInfo, &acts, &n)) && acts) {
                for (UINT32 i = 0; i < n; ++i) {
                    if (!acts[i]) continue;
                    found = true;
                    WCHAR* nm = nullptr;
                    UINT32 len = 0;
                    if (SUCCEEDED(acts[i]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &nm, &len)) && nm) {
                        if (!e.names.empty()) e.names += "; ";
                        if (e.names.size() < 200) e.names += narrowUtf8(nm);
                        CoTaskMemFree(nm);
                    }
                    acts[i]->Release();
                }
                CoTaskMemFree(acts);
            }
        };
        enumerate(MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER, e.hardwareH264);
        enumerate(MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER, e.softwareH264);
        e.known = true;
        MFShutdown();
    }
    if (comOwned) CoUninitialize();
    return e;
}

}  // namespace

SystemInfo querySystemInfo() {
    SystemInfo s;
    try {
        s.windowsVersion = windowsVersionText();
        s.cpuName = cpuNameText();
        s.gpus = gpuNames();
        EnumDisplayMonitors(nullptr, nullptr, monitorCb, reinterpret_cast<LPARAM>(&s.monitors));
    } catch (...) {
    }
    return s;
}

Probes makeProbes() {
    Probes p;
    p.freeDiskBytes = [](const std::string& folder) { return freeDisk(folder); };
    p.reachability = [] { return reachability(); };
    p.uploadTest = [](const std::string& url) { return uploadTest(url); };
    p.encoders = [] { return encoders(); };
    p.nowEpochSec = [] {
        return static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    };
    return p;
}

}  // namespace hb::diag::sys
