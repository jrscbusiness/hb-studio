#include "app/windows/win_util.hpp"

#include <objbase.h>
#include <shlobj.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <exception>

#include "app/json_escape.hpp"

namespace hb::win {

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

bool loadResource(int id, const void** data, std::size_t* size) {
    HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!r) return false;
    HGLOBAL g = LoadResource(nullptr, r);
    if (!g) return false;
    *data = LockResource(g);
    *size = SizeofResource(nullptr, r);
    return *data != nullptr;
}

std::string loadResourceText(int id) {
    const void* d = nullptr;
    std::size_t n = 0;
    if (!loadResource(id, &d, &n)) return {};
    return std::string(static_cast<const char*>(d), n);
}

std::string base64(const std::uint8_t* d, std::size_t n) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((n + 2) / 3 * 4);
    for (std::size_t i = 0; i < n; i += 3) {
        const std::uint32_t v = (d[i] << 16) | (i + 1 < n ? d[i + 1] << 8 : 0) | (i + 2 < n ? d[i + 2] : 0);
        out += t[(v >> 18) & 63];
        out += t[(v >> 12) & 63];
        out += i + 1 < n ? t[(v >> 6) & 63] : '=';
        out += i + 2 < n ? t[v & 63] : '=';
    }
    return out;
}

std::string jsonEscape(const std::string& s) { return hb::app::jsonEscape(s); }

std::wstring localAppDataPath() {
    PWSTR p = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p))) out = p;
    CoTaskMemFree(p);   // safe on nullptr
    return out;
}

unsigned windowsBuild() {
    using Fn = LONG(WINAPI*)(OSVERSIONINFOW*);
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    auto fn = nt ? reinterpret_cast<Fn>(GetProcAddress(nt, "RtlGetVersion")) : nullptr;
    if (!fn) return 0;
    OSVERSIONINFOW vi{};
    vi.dwOSVersionInfoSize = sizeof(vi);
    return fn(&vi) == 0 ? vi.dwBuildNumber : 0;
}

bool systemPrefersDark() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", 0,
                      KEY_READ, &key) != ERROR_SUCCESS)
        return false;
    DWORD value = 1, size = sizeof(value), type = 0;
    const LSTATUS r = RegQueryValueExW(key, L"AppsUseLightTheme", nullptr, &type, reinterpret_cast<BYTE*>(&value), &size);
    RegCloseKey(key);
    return r == ERROR_SUCCESS && type == REG_DWORD && value == 0;
}

namespace {
wchar_t g_dataDir[MAX_PATH] = {0};   // set by installCrashReporter
volatile LONG g_startupLogReset = 0;

// "HBStudio.exe+0x1234" for an address, or "?+0x..." when no module owns it. Async-signal-ish: only Win32 calls.
void describeAddress(const void* addr, char* out, std::size_t cap) {
    HMODULE m = nullptr;
    wchar_t name[MAX_PATH] = {0};
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCWSTR>(addr), &m) &&
        m && GetModuleFileNameW(m, name, MAX_PATH)) {
        const wchar_t* base = name;
        for (const wchar_t* p = name; *p; ++p)
            if (*p == L'\\' || *p == L'/') base = p + 1;
        char narrowName[128] = {0};
        WideCharToMultiByte(CP_UTF8, 0, base, -1, narrowName, static_cast<int>(sizeof narrowName) - 1, nullptr, nullptr);
        snprintf(out, cap, "%s+0x%llx", narrowName,
                 static_cast<unsigned long long>(static_cast<const char*>(addr) - reinterpret_cast<const char*>(m)));
    } else {
        snprintf(out, cap, "?+0x%llx", static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(addr)));
    }
}

void appendFile(const wchar_t* path, DWORD disposition, const char* data, std::size_t len) {
    HANDLE f = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, disposition,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    WriteFile(f, data, static_cast<DWORD>(len), &w, nullptr);
    CloseHandle(f);
}

void writeCrashReport(const char* title, DWORD code, const void* faultAddr) {
    if (g_dataDir[0] == 0) return;
    char text[6144];
    int n = 0;
    char where[200];
    describeAddress(faultAddr, where, sizeof where);
    SYSTEMTIME st;
    GetLocalTime(&st);
    n += snprintf(text + n, sizeof text - static_cast<std::size_t>(n),
                  "HB Studio crash report\r\n%s\r\nversion: %s\r\ntime: %04d-%02d-%02d %02d:%02d:%02d\r\nthread: %lu\r\n"
                  "exception code: 0x%08lX\r\nfault: %s\r\nstack (module+offset):\r\n",
                  title,
#ifdef HB_VERSION
                  HB_VERSION,
#else
                  "?",
#endif
                  st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, GetCurrentThreadId(),
                  static_cast<unsigned long>(code), where);
    void* frames[48] = {nullptr};
    const USHORT count = CaptureStackBackTrace(0, 48, frames, nullptr);
    for (USHORT i = 0; i < count && n < static_cast<int>(sizeof text) - 220; ++i) {
        describeAddress(frames[i], where, sizeof where);
        n += snprintf(text + n, sizeof text - static_cast<std::size_t>(n), "  %s\r\n", where);
    }
    wchar_t path[MAX_PATH];
    _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\crash-%04d%02d%02d-%02d%02d%02d.txt", g_dataDir, st.wYear, st.wMonth,
                 st.wDay, st.wHour, st.wMinute, st.wSecond);
    appendFile(path, CREATE_ALWAYS, text, static_cast<std::size_t>(n));
    wchar_t logPath[MAX_PATH];
    _snwprintf_s(logPath, MAX_PATH, _TRUNCATE, L"%s\\startup.log", g_dataDir);
    char line[300];
    const int ln = snprintf(line, sizeof line, "CRASH %s code=0x%08lX fault=%s\r\n", title, static_cast<unsigned long>(code), where);
    appendFile(logPath, OPEN_ALWAYS, line, static_cast<std::size_t>(ln));
}

LONG WINAPI crashFilter(EXCEPTION_POINTERS* ep) {
    if (ep && ep->ExceptionRecord)
        writeCrashReport("unhandled exception", ep->ExceptionRecord->ExceptionCode, ep->ExceptionRecord->ExceptionAddress);
    return EXCEPTION_CONTINUE_SEARCH;   // let Windows report/terminate as usual
}

void terminateHandler() {
    char what[200] = "unknown";
    try {
        if (auto e = std::current_exception()) std::rethrow_exception(e);
    } catch (const std::exception& ex) {
        snprintf(what, sizeof what, "%s", ex.what());
    } catch (...) {
    }
    char title[260];
    snprintf(title, sizeof title, "std::terminate (uncaught C++ exception: %s)", what);
    writeCrashReport(title, 0xE06D7363, reinterpret_cast<const void*>(&terminateHandler));
    std::abort();
}
}  // namespace

void installCrashReporter(const std::wstring& dir) {
    if (dir.empty() || dir.size() >= MAX_PATH - 40) return;
    wcsncpy_s(g_dataDir, MAX_PATH, dir.c_str(), _TRUNCATE);
    SetUnhandledExceptionFilter(crashFilter);
    std::set_terminate(terminateHandler);
}

void trace(const char* msg) {
    wchar_t path[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(L"HB_TRACE", path, MAX_PATH);
    const bool haveEnv = n != 0 && n < MAX_PATH;
    DWORD disposition = OPEN_ALWAYS;
    if (!haveEnv) {
        if (g_dataDir[0] == 0) return;
        _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\startup.log", g_dataDir);
        // First line of this process truncates the previous run's log.
        if (InterlockedCompareExchange(&g_startupLogReset, 1, 0) == 0) disposition = CREATE_ALWAYS;
    }
    HANDLE f = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, disposition,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    char line[512];
    const int len = snprintf(line, sizeof line, "%llu t%lu %s\r\n", static_cast<unsigned long long>(GetTickCount64()),
                             GetCurrentThreadId(), msg);
    DWORD w = 0;
    WriteFile(f, line, static_cast<DWORD>(len), &w, nullptr);
    CloseHandle(f);
}

}  // namespace hb::win
