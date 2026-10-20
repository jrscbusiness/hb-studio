// HBStudio.exe - main application: native window + WebView2 UI + engine.
//
//   HBStudio.exe                    normal start
//   HBStudio.exe --screenshot X.png renders the UI, saves a PNG and exits (CI / support)
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "app/engine.hpp"
#include "app/ptz_settings.hpp"
#include "app/windows/resource_ids.h"
#include "app/windows/vcam_controller.hpp"
#include "app/windows/webview_host.hpp"
#include "app/windows/wic_jpeg.hpp"
#include "app/windows/win_util.hpp"

using namespace hb;

namespace {

std::string stateJson(app::Engine& engine, win::VirtualCameraController& vcam, int toastId, const std::string& toast) {
    const auto s = engine.snapshot();
    const auto v = vcam.status();
    char b[2048];
    snprintf(b, sizeof b,
             "{\"t\":\"s\",\"pvw\":%d,\"pgm\":%d,\"transition\":%s,\"progress\":%.3f,\"fps\":%.2f,\"target\":%d,"
             "\"frames\":%llu,\"skipped\":%llu,\"tier\":\"%s\",\"cores\":%u,\"ramGiB\":%u,\"srcRec\":%d,\"srcMax\":%d,"
             "\"vcam\":{\"supported\":%s,\"installed\":%s,\"active\":%s,\"connected\":%s,\"busy\":%s,\"error\":%s,"
             "\"sent\":%llu,\"message\":\"%s\"}",
             s.sw.pvw, s.sw.pgm, s.sw.transitioning ? "true" : "false", s.sw.progress, s.fps, s.targetFps,
             static_cast<unsigned long long>(s.produced), static_cast<unsigned long long>(s.skipped), s.tier.c_str(),
             s.cores, s.ramGiB, s.sourcesRecommended, s.sourcesMax, v.supported ? "true" : "false",
             v.installed ? "true" : "false", v.active ? "true" : "false", v.connected ? "true" : "false",
             v.busy ? "true" : "false", v.error ? "true" : "false", static_cast<unsigned long long>(v.sent),
             win::jsonEscape(v.message).c_str());
    std::string j = b;
    j += toastId > 0 ? ",\"toast\":{\"id\":" + std::to_string(toastId) + ",\"text\":\"" + win::jsonEscape(toast) + "\"}"
                     : std::string(",\"toast\":null");
    j += ",\"log\":[";
    auto tail = engine.logTail();
    const std::size_t from = tail.size() > 9 ? tail.size() - 9 : 0;
    for (std::size_t i = from; i < tail.size(); ++i) {
        const auto& r = tail[i];
        const std::time_t t = std::chrono::system_clock::to_time_t(r.time);
        std::tm tm{};
        localtime_s(&tm, &t);
        char line[512];
        snprintf(line, sizeof line, "%02d:%02d:%02d [%s] [%s] %s", tm.tm_hour, tm.tm_min, tm.tm_sec,
                 core::levelName(r.level), r.module.c_str(), r.message.c_str());
        if (i > from) j += ",";
        j += "{\"t\":\"" + win::jsonEscape(line) + "\",\"l\":\"" + core::levelName(r.level) + "\"}";
    }
    j += "]}";
    return j;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    std::wstring screenshotPath;
    {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        for (int i = 1; argv && i + 1 < argc; ++i)
            if (std::wstring(argv[i]) == L"--screenshot") screenshotPath = argv[i + 1];
        if (argv) LocalFree(argv);
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    app::Engine engine;
    engine.start();
    win::VirtualCameraController vcam(engine.program(), engine.logger());

    std::wstring dataDir;
    {
        PWSTR p = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p))) {
            dataDir = std::wstring(p) + L"\\HBStudio\\WebView2";
            CoTaskMemFree(p);
        }
    }

    win::WebViewHost host;
    std::atomic<bool> running{true};
    // ---- PTZ settings (names + per-camera configuration), persisted next to the WebView2 data ----
    app::PtzSettings ptzSettings;
    std::string ptzPath;
    {
        PWSTR p = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p))) {
            const std::wstring dir = std::wstring(p) + L"\\HBStudio";
            CoTaskMemFree(p);
            SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
            ptzPath = win::narrow(dir + L"\\ptz.json");
        }
        if (!ptzPath.empty() && !ptzSettings.load(ptzPath))
            engine.logger().info("ptz", "sem configuração anterior: usando padrões");
    }
    std::atomic<unsigned> ptzSent{0};
    std::mutex toastMutex;
    int toastId = 0;
    std::string toastText;
    auto toast = [&](const std::string& t) {
        std::lock_guard lock(toastMutex);
        toastText = t;
        ++toastId;
    };
    auto ptzChanged = [&] {
        if (!ptzPath.empty() && !ptzSettings.save(ptzPath)) engine.logger().warning("ptz", "não foi possível salvar a configuração");
        ptzSent = 0;  // force the feeder to resend
    };
    constexpr const char* kNoPtz = "nenhuma câmera PTZ conectada (disponível nas Fases 4–5). Nada foi enviado.";

    // Splits "cmd|a|b|rest": at most `maxParts` parts, the last one keeps any further '|'.
    auto split = [](const std::string& msg, std::size_t maxParts) {
        std::vector<std::string> a;
        std::size_t s0 = 0;
        for (std::size_t i = 0; i <= msg.size(); ++i)
            if (i == msg.size() || (msg[i] == '|' && a.size() + 1 < maxParts)) { a.push_back(msg.substr(s0, i - s0)); s0 = i + 1; }
        return a;
    };

    auto onMessage = [&](const std::string& msg) {
        const std::string c0 = msg.substr(0, msg.find('|'));
        const std::size_t maxParts = c0 == "ptzname" ? 4 : (c0 == "ptzcfg" ? 3 : (c0 == "ptzall" ? 2 : 8));
        auto a = split(msg, maxParts);
        if (a.empty()) return;
        auto idx = [&](std::size_t k) { return k < a.size() ? std::atoi(a[k].c_str()) : -1; };
        const std::string& c = a[0];
        const int cam = idx(1);
        const std::string camLabel = "CAM " + std::to_string(cam + 1) + ": ";
        if (c == "ready") ptzSent = 0;
        else if (c == "preview") engine.switcher().setPreview(idx(1));
        else if (c == "program") engine.switcher().setProgram(idx(1));
        else if (c == "cut") engine.switcher().cut();
        else if (c == "dissolve") engine.switcher().startDissolve(idx(1) > 0 ? idx(1) : 500);
        else if (c == "vcam" && a.size() > 1) {
            if (a[1] == "activate") vcam.activate();
            else if (a[1] == "deactivate") vcam.deactivate();
        } else if (c == "ptzname" && a.size() >= 4) {
            if (ptzSettings.setPresetName(cam, idx(2), a[3])) ptzChanged();
        } else if (c == "ptzcfg" && a.size() >= 3) {
            app::PtzCameraConfig cfg;
            if (app::PtzSettings::parseConfig(a[2], cfg) && ptzSettings.setConfig(cam, cfg)) {
                ptzChanged();
                toast(camLabel + "configuração PTZ guardada.");
            }
        } else if (c == "ptzall" && a.size() >= 2) {
            app::PtzCameraConfig cfg;
            if (app::PtzSettings::parseConfig(a[1], cfg)) {
                ptzSettings.applyToAll(cfg);
                ptzChanged();
                toast("Configuração PTZ aplicada a todas as câmeras.");
            }
        } else if (c == "ptzgo" || c == "ptzsave" || c == "ptzmove") {
            // No PTZ provider is connected yet (Phases 4-5): say so instead of pretending.
            toast(camLabel + kNoPtz);
        }
        // ptzstop / ptzzoom / ptzfocus: silent no-ops until a provider exists.
    };

    win::WebViewHost::Options opt;
    opt.userDataDir = dataDir;
    opt.title = L"HB Studio";

    std::thread feeder;
    auto startFeeder = [&] {
        feeder = std::thread([&] {
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            win::JpegEncoder jpeg;
            jpeg.init();
            video::Frame frame;
            std::vector<std::uint8_t> bytes;
            std::uint64_t tick = 0;
            auto next = std::chrono::steady_clock::now();
            while (running) {
                next += std::chrono::milliseconds(66);  // ~15 Hz
                auto sendFrame = [&](const char* id, int quality) {
                    if (!engine.renderUiFrame(id, frame) || !jpeg.encode(frame, quality, bytes)) return;
                    host.postQueued(id, std::string("{\"t\":\"f\",\"id\":\"") + id + "\",\"d\":\"" +
                                            win::base64(bytes.data(), bytes.size()) + "\"}");
                };
                sendFrame("pgm", 80);
                sendFrame("pvw", 80);
                // Input tiles: half of them per tick (~7.5 fps each) to save CPU.
                for (int i = static_cast<int>(tick % 2); i < app::Engine::kSourceCount; i += 2)
                    sendFrame(("s" + std::to_string(i)).c_str(), 70);
                {
                    int tid; std::string ttxt;
                    { std::lock_guard lock(toastMutex); tid = toastId; ttxt = toastText; }
                    host.postQueued("state", stateJson(engine, vcam, tid, ttxt));
                    const unsigned v = ptzSettings.version();
                    if (ptzSent.load() != v) { ptzSent = v; host.postQueued("ptz", ptzSettings.uiJson()); }
                }
                ++tick;
                std::this_thread::sleep_until(next);
            }
            CoUninitialize();
        });
    };

    const bool shot = !screenshotPath.empty();
    if (!host.create(inst, opt, win::loadResourceText(IDR_UI_HTML), onMessage, [&] {
            startFeeder();
            if (shot) {
                // Let a few frames arrive, then capture.
                std::thread([&] {
                    std::this_thread::sleep_for(std::chrono::seconds(4));
                    PostMessageW(host.hwnd(), WM_APP + 2, 0, 0);
                }).detach();
            }
        })) {
        MessageBoxW(nullptr, L"Não foi possível criar a janela.", L"HB Studio", MB_ICONERROR);
        return 1;
    }
    if (shot) {
        // Poll for the capture trigger inside the normal message loop.
        SetTimer(host.hwnd(), 1, 250, [](HWND, UINT, UINT_PTR, DWORD) {});
    }

    int rc;
    if (shot) {
        MSG msg;
        bool captured = false;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            if (msg.message == WM_APP + 2 && !captured) {
                captured = true;
                host.screenshot(screenshotPath, [&](bool) { host.close(); });
                continue;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        rc = static_cast<int>(msg.wParam);
    } else {
        rc = host.run();
    }

    running = false;
    if (feeder.joinable()) feeder.join();
    vcam.shutdown();
    engine.stop();
    CoUninitialize();
    return rc;
}
