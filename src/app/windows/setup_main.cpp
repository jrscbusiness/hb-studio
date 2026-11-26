// HBStudio-Setup.exe - graphical installer / uninstaller (one binary, two modes).
//
// The installer embeds HBStudio.exe and hb_vcam_source.dll (and, when built with ONNX Runtime, onnxruntime.dll
// plus its MIT notices) as resources. It runs
// elevated (manifest: requireAdministrator) because it writes to Program Files and
// registers the virtual camera media source under HKLM. The same binary is copied to
// the install folder as Uninstall.exe and started with --uninstall from
// "Apps installed" in Windows Settings.
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cwchar>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "app/windows/resource_ids.h"
#include "app/windows/webview_host.hpp"
#include "app/windows/win_util.hpp"

#ifndef HB_VERSION
#define HB_VERSION "0.2.0"
#endif

using namespace hb;

namespace {

constexpr wchar_t kUninstallKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\HBStudio";

std::wstring knownFolder(const KNOWNFOLDERID& id) {
    PWSTR p = nullptr;
    std::wstring r;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) {
        r = p;
        CoTaskMemFree(p);
    }
    return r;
}

std::wstring installDir() { return knownFolder(FOLDERID_ProgramFiles) + L"\\HB Studio"; }

std::wstring lastError(const wchar_t* what) {
    const DWORD e = GetLastError();
    wchar_t* buf = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                   e, MAKELANGID(LANG_PORTUGUESE, SUBLANG_PORTUGUESE_BRAZILIAN), reinterpret_cast<LPWSTR>(&buf), 0,
                   nullptr);
    std::wstring s = std::wstring(what) + L" (erro " + std::to_wstring(e) + L"): " + (buf ? buf : L"");
    if (buf) LocalFree(buf);
    while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r' || s.back() == L' ')) s.pop_back();
    return s;
}

bool writeResource(int id, const std::wstring& path, std::wstring& err) {
    const void* data = nullptr;
    std::size_t size = 0;
    if (!win::loadResource(id, &data, &size)) { err = L"Recurso embutido não encontrado."; return false; }
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        err = lastError((L"Não foi possível gravar " + path).c_str());
        if (GetLastError() == ERROR_SHARING_VIOLATION) err += L"\nFeche o HB Studio e tente novamente.";
        return false;
    }
    DWORD written = 0;
    const bool ok = WriteFile(f, data, static_cast<DWORD>(size), &written, nullptr) && written == size;
    if (!ok) err = lastError(L"Falha ao gravar o arquivo");
    CloseHandle(f);
    return ok;
}

// Optional embedded resource (e.g. onnxruntime.dll): absent when the installer was built without ONNX Runtime.
bool hasResource(int id) {
    const void* d = nullptr;
    std::size_t n = 0;
    return win::loadResource(id, &d, &n) && n > 0;
}

std::size_t resourceSize(int id) {
    const void* d = nullptr;
    std::size_t n = 0;
    return win::loadResource(id, &d, &n) ? n : 0;
}

std::wstring system32() {
    wchar_t b[MAX_PATH];
    const UINT n = GetSystemDirectoryW(b, MAX_PATH);
    return n && n < MAX_PATH ? std::wstring(b) : L"C:\\Windows\\System32";
}

// Runs a system tool by ABSOLUTE path (an elevated installer must never resolve "icacls"
// or "cmd" from its own folder). Returns the exit code, -1 if it could not start.
int runHidden(const std::wstring& tool, const std::wstring& args) {
    std::wstring cmd = L"\"" + system32() + L"\\" + tool + L"\" " + args;
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return -1;
    WaitForSingleObject(pi.hProcess, 20000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return static_cast<int>(code);
}

// Frame Server keeps the media source DLL loaded; stop it so files can be replaced/removed.
void stopFrameServer() {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return;
    SC_HANDLE svc = OpenServiceW(scm, L"FrameServer", SERVICE_STOP | SERVICE_QUERY_STATUS);
    if (svc) {
        SERVICE_STATUS st{};
        ControlService(svc, SERVICE_CONTROL_STOP, &st);
        for (int i = 0; i < 40; ++i) {
            if (!QueryServiceStatus(svc, &st) || st.dwCurrentState == SERVICE_STOPPED) break;
            Sleep(200);
        }
        CloseServiceHandle(svc);
    }
    CloseServiceHandle(scm);
}

// Calls DllRegisterServer / DllUnregisterServer of the media source DLL.
HRESULT registerDll(const std::wstring& dll, bool reg) {
    HMODULE m = LoadLibraryW(dll.c_str());
    if (!m) return HRESULT_FROM_WIN32(GetLastError());
    using Fn = HRESULT(STDAPICALLTYPE*)();
    auto fn = reinterpret_cast<Fn>(GetProcAddress(m, reg ? "DllRegisterServer" : "DllUnregisterServer"));
    HRESULT hr = fn ? fn() : HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    FreeLibrary(m);
    return hr;
}

bool createShortcut(const std::wstring& link, const std::wstring& target, const std::wstring& workDir) {
    IShellLinkW* sl = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl)))) return false;
    sl->SetPath(target.c_str());
    sl->SetWorkingDirectory(workDir.c_str());
    sl->SetDescription(L"HB Studio — produção de vídeo ao vivo");
    sl->SetIconLocation(target.c_str(), 0);
    IPersistFile* pf = nullptr;
    bool ok = false;
    if (SUCCEEDED(sl->QueryInterface(IID_PPV_ARGS(&pf)))) {
        ok = SUCCEEDED(pf->Save(link.c_str(), TRUE));
        pf->Release();
    }
    sl->Release();
    return ok;
}

void writeUninstallEntry(const std::wstring& dir) {
    // ~12 MB of app + the optional onnxruntime.dll (bundled for the AI chroma key).
    const DWORD estimatedKb = static_cast<DWORD>(12000 + resourceSize(IDR_ORT_DLL) / 1024);
    HKEY k;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kUninstallKey, 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS)
        return;
    auto str = [&](const wchar_t* n, const std::wstring& v) {
        RegSetValueExW(k, n, 0, REG_SZ, reinterpret_cast<const BYTE*>(v.c_str()),
                       static_cast<DWORD>((v.size() + 1) * sizeof(wchar_t)));
    };
    auto dw = [&](const wchar_t* n, DWORD v) { RegSetValueExW(k, n, 0, REG_DWORD, reinterpret_cast<BYTE*>(&v), sizeof v); };
    str(L"DisplayName", L"HB Studio");
    str(L"DisplayVersion", win::widen(HB_VERSION));
    str(L"Publisher", L"HB Studio (software livre)");
    str(L"InstallLocation", dir);
    str(L"DisplayIcon", dir + L"\\HBStudio.exe");
    str(L"UninstallString", L"\"" + dir + L"\\Uninstall.exe\" --uninstall");
    dw(L"NoModify", 1);
    dw(L"NoRepair", 1);
    dw(L"EstimatedSize", estimatedKb);
    RegCloseKey(k);
}

struct Options { bool desktop = true; bool vcam = true; };

Options parseOptions(const std::string& msg) {
    Options o;
    if (msg.find("desktop=0") != std::string::npos) o.desktop = false;
    if (msg.find("vcam=0") != std::string::npos) o.vcam = false;
    return o;
}

}  // namespace

class Setup {
public:
    explicit Setup(win::WebViewHost& host, bool uninstall) : host_(host), uninstall_(uninstall) {}
    ~Setup() { if (worker_.joinable()) worker_.join(); }

    void onMessage(const std::string& msg) {
        const std::string cmd = msg.substr(0, msg.find('|'));
        if (cmd == "ready") {
            host_.postQueued("", std::string("{\"t\":\"mode\",\"mode\":\"") + (uninstall_ ? "uninstall" : "install") +
                                     "\",\"version\":\"" HB_VERSION "\",\"path\":\"" +
                                     win::jsonEscape(win::narrow(installDir())) + "\"}");
        } else if ((cmd == "install" || cmd == "uninstall") && !running_.exchange(true)) {
            const Options o = parseOptions(msg);
            if (worker_.joinable()) worker_.join();
            worker_ = std::thread([this, o] {
                CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
                std::wstring err;
                const bool ok = uninstall_ ? doUninstall(err) : doInstall(o, err);
                done(ok, err);
                running_ = false;   // allow "try again" after a failure
                CoUninitialize();
            });
        } else if (cmd == "launch") {
            const std::wstring exe = L"\"" + installDir() + L"\\HBStudio.exe\"";
            // Through Explorer so the app starts with the normal (non-elevated) user token.
            ShellExecuteW(nullptr, L"open", L"explorer.exe", exe.c_str(), nullptr, SW_SHOWNORMAL);
            host_.close();
        } else if (cmd == "close") {
            host_.close();
        }
    }

private:
    void step(int i, int total, const std::wstring& label, const char* state = "cur") {
        host_.postQueued("", "{\"t\":\"step\",\"i\":" + std::to_string(i) + ",\"label\":\"" +
                                 win::jsonEscape(win::narrow(label)) + "\",\"pct\":" +
                                 std::to_string(i * 100 / total) + ",\"state\":\"" + state + "\"}");
    }
    void done(bool ok, const std::wstring& msg) {
        host_.postQueued("", std::string("{\"t\":\"done\",\"ok\":") + (ok ? "true" : "false") + ",\"msg\":\"" +
                                 win::jsonEscape(win::narrow(msg)) + "\"}");
    }

    bool doInstall(const Options& o, std::wstring& err) {
        const std::wstring dir = installDir();
        const bool vcam = o.vcam && win::windowsBuild() >= 22000;
        const int total = vcam ? 6 : 5;
        int i = 0;

        step(i, total, L"Preparando a instalação");
        if (win::windowsBuild() >= 22000) stopFrameServer();
        SHCreateDirectoryExW(nullptr, (dir + L"\\VirtualCamera").c_str(), nullptr);

        step(++i, total, L"Copiando arquivos");
        if (!writeResource(IDR_APP_EXE, dir + L"\\HBStudio.exe", err)) { step(i, total, L"Copiando arquivos", "bad"); return false; }
        wchar_t self[MAX_PATH];
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        if (_wcsicmp(self, (dir + L"\\Uninstall.exe").c_str()) != 0 &&
            !CopyFileW(self, (dir + L"\\Uninstall.exe").c_str(), FALSE)) {
            err = lastError(L"Não foi possível criar o desinstalador");
            return false;
        }

        // AI chroma key runtime (optional): onnxruntime.dll must sit next to HBStudio.exe (the app delay-loads it).
        // A failure here is only a warning: the rest of the app works without it.
        std::wstring ortWarn;
        if (hasResource(IDR_ORT_DLL)) {
            std::wstring e2;
            if (!writeResource(IDR_ORT_DLL, dir + L"\\onnxruntime.dll", e2))
                ortWarn = L"Aviso: não foi possível instalar o onnxruntime.dll; o chroma por IA ficará indisponível. " + e2;
            const std::wstring licDir = dir + L"\\licenses";
            SHCreateDirectoryExW(nullptr, licDir.c_str(), nullptr);
            std::wstring ignore;
            if (hasResource(IDR_ORT_LICENSE)) writeResource(IDR_ORT_LICENSE, licDir + L"\\onnxruntime-LICENSE.txt", ignore);
            if (hasResource(IDR_ORT_NOTICES)) writeResource(IDR_ORT_NOTICES, licDir + L"\\onnxruntime-ThirdPartyNotices.txt", ignore);
        }

        std::wstring extra;
        if (vcam) {
            const std::wstring dll = dir + L"\\VirtualCamera\\hb_vcam_source.dll";
            if (!writeResource(IDR_VCAM_DLL, dll, err)) { step(i, total, L"Copiando arquivos", "bad"); return false; }

            step(++i, total, L"Configurando permissões");
            // Frame Server runs as LocalService: it must read the DLL; it also writes a diagnostic log.
            if (runHidden(L"icacls.exe", L"\"" + dir + L"\\VirtualCamera\" /grant *S-1-5-19:(OI)(CI)RX /T /C") != 0) {
                err = L"Não foi possível aplicar as permissões da câmera virtual (icacls).";
                step(i, total, L"Configurando permissões", "bad");
                return false;
            }
            const std::wstring logDir = knownFolder(FOLDERID_ProgramData) + L"\\HBStudio";
            SHCreateDirectoryExW(nullptr, logDir.c_str(), nullptr);
            // LocalService (the Frame Server) writes the diagnostic log; ordinary users may only read it.
            runHidden(L"icacls.exe", L"\"" + logDir + L"\" /inheritance:r /grant:r *S-1-5-19:(OI)(CI)M *S-1-5-32-544:(OI)(CI)F *S-1-5-18:(OI)(CI)F *S-1-5-32-545:(OI)(CI)RX /C");

            step(++i, total, L"Registrando a câmera virtual");
            const HRESULT hr = registerDll(dll, true);
            if (FAILED(hr)) {
                wchar_t b[96];
                swprintf_s(b, L"Falha ao registrar a câmera virtual (0x%08lX).", static_cast<unsigned long>(hr));
                err = b;
                step(i, total, L"Registrando a câmera virtual", "bad");
                return false;
            }
        }

        step(++i, total, L"Criando atalhos");
        const std::wstring menu = knownFolder(FOLDERID_CommonPrograms) + L"\\HB Studio";
        SHCreateDirectoryExW(nullptr, menu.c_str(), nullptr);
        createShortcut(menu + L"\\HB Studio.lnk", dir + L"\\HBStudio.exe", dir);
        if (o.desktop) createShortcut(knownFolder(FOLDERID_PublicDesktop) + L"\\HB Studio.lnk", dir + L"\\HBStudio.exe", dir);

        step(++i, total, L"Finalizando");
        writeUninstallEntry(dir);
        if (!ortWarn.empty()) err = ortWarn;
        if (o.vcam && !vcam)
            err = L"Aviso: a câmera virtual exige Windows 11 e não foi instalada neste computador. O restante funciona normalmente.";
        step(total, total, L"Finalizando", "ok");
        return true;
    }

    bool doUninstall(std::wstring& err) {
        const std::wstring dir = installDir();
        const std::wstring dll = dir + L"\\VirtualCamera\\hb_vcam_source.dll";
        const int total = 4;

        step(0, total, L"Encerrando componentes");
        runHidden(L"taskkill.exe", L"/F /IM HBStudio.exe");
        if (win::windowsBuild() >= 22000) stopFrameServer();

        step(1, total, L"Removendo a câmera virtual");
        bool unregistered = true;
        if (GetFileAttributesW(dll.c_str()) != INVALID_FILE_ATTRIBUTES) unregistered = SUCCEEDED(registerDll(dll, false));

        step(2, total, L"Removendo arquivos");
        if (unregistered) DeleteFileW(dll.c_str());
        else err = L"A câmera virtual não pôde ser desregistrada; o arquivo foi mantido para evitar um registro quebrado. Reinicie o computador e tente remover de novo.";
        RemoveDirectoryW((dir + L"\\VirtualCamera").c_str());
        DeleteFileW((dir + L"\\HBStudio.exe").c_str());
        DeleteFileW((dir + L"\\onnxruntime.dll").c_str());
        DeleteFileW((dir + L"\\licenses\\onnxruntime-LICENSE.txt").c_str());
        DeleteFileW((dir + L"\\licenses\\onnxruntime-ThirdPartyNotices.txt").c_str());
        RemoveDirectoryW((dir + L"\\licenses").c_str());

        step(3, total, L"Removendo atalhos e registro");
        DeleteFileW((knownFolder(FOLDERID_CommonPrograms) + L"\\HB Studio\\HB Studio.lnk").c_str());
        RemoveDirectoryW((knownFolder(FOLDERID_CommonPrograms) + L"\\HB Studio").c_str());
        DeleteFileW((knownFolder(FOLDERID_PublicDesktop) + L"\\HB Studio.lnk").c_str());
        RegDeleteTreeW(HKEY_LOCAL_MACHINE, kUninstallKey);

        // This process is Uninstall.exe: it cannot delete itself while running.
        const std::wstring self = dir + L"\\Uninstall.exe";
        MoveFileExW(self.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
        std::wstring cmd = L"\"" + system32() + L"\\cmd.exe\" /c ping 127.0.0.1 -n 4 >nul & del /f /q \"" + self + L"\" & rmdir /q \"" + dir + L"\"";
        STARTUPINFOW si{sizeof(si)};
        PROCESS_INFORMATION pi{};
        if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr,
                           nullptr, &si, &pi)) {
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        }
        step(total, total, L"Removendo atalhos e registro", "ok");
        return unregistered;
    }

    win::WebViewHost& host_;
    bool uninstall_;
    std::atomic<bool> running_{false};
    std::thread worker_;
};

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    bool uninstall = false;
    std::wstring screenshot;
    {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        for (int i = 1; argv && i < argc; ++i) {
            if (std::wstring(argv[i]) == L"--uninstall") uninstall = true;
            if (std::wstring(argv[i]) == L"--screenshot" && i + 1 < argc) screenshot = argv[i + 1];
        }
        if (argv) LocalFree(argv);
        wchar_t self[MAX_PATH];
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        const std::wstring name = wcsrchr(self, L'\\') ? wcsrchr(self, L'\\') + 1 : self;
        if (_wcsicmp(name.c_str(), L"Uninstall.exe") == 0) uninstall = true;
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    win::WebViewHost host;
    Setup setup(host, uninstall);

    win::WebViewHost::Options opt;
    opt.title = uninstall ? L"Remover o HB Studio" : L"Instalar o HB Studio";
    opt.width = 720;
    opt.height = 600;
    opt.minWidth = 720;
    opt.minHeight = 600;
    opt.resizable = false;
    opt.userDataDir = knownFolder(FOLDERID_LocalAppData) + L"\\HBStudioSetup\\WebView2";
    if (opt.userDataDir.empty() || opt.userDataDir[0] == L'\\') {
        wchar_t tmp[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        opt.userDataDir = std::wstring(tmp) + L"HBStudioSetup\\WebView2";
    }

    if (!host.create(inst, opt, win::loadResourceText(IDR_SETUP_HTML),
                     [&](const std::string& m) { setup.onMessage(m); }, [&] {
                         if (!screenshot.empty()) {
                             std::thread([&] {
                                 std::this_thread::sleep_for(std::chrono::seconds(2));
                                 PostMessageW(host.hwnd(), WM_APP + 2, 0, 0);
                             }).detach();
                         }
                     })) {
        MessageBoxW(nullptr, L"Não foi possível criar a janela do instalador.", L"HB Studio", MB_ICONERROR);
        return 1;
    }

    int rc;
    if (!screenshot.empty()) {
        MSG msg;
        bool captured = false;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            if (msg.message == WM_APP + 2 && !captured) {
                captured = true;
                host.screenshot(screenshot, [&](bool) { host.close(); });
                continue;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        rc = static_cast<int>(msg.wParam);
    } else {
        rc = host.run();
    }
    CoUninitialize();
    return rc;
}
