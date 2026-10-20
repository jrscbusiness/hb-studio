// HBStudio.exe - main application: native window + WebView2 UI + engine.
//
//   HBStudio.exe                    normal start
//   HBStudio.exe --screenshot X.png renders the UI, saves a PNG and exits (CI / support)
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "app/engine.hpp"
#include "app/ptz_settings.hpp"
#include "app/source_assignments.hpp"
#include "app/windows/dshow_capture.hpp"
#include "app/windows/task_queue.hpp"
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
    j += ",\"slots\":[";
    for (int i = 0; i < app::Engine::kSourceCount; ++i) {
        const auto info = engine.slotInfo(i);
        const char* st = "test";
        if (info.live) {
            using S = video::SourceStatus::State;
            st = info.status.state == S::Online ? "online" : info.status.state == S::Connecting ? "connecting"
               : info.status.state == S::NoSignal ? "nosignal" : "error";
        }
        char sb[160];
        snprintf(sb, sizeof sb, "%s{\"k\":\"%s\",\"st\":\"%s\",\"w\":%d,\"h\":%d,\"fps\":%.1f,\"name\":\"", i ? "," : "",
                 info.live ? "dev" : "test", st, info.status.width, info.status.height, info.status.fps);
        j += sb + win::jsonEscape(info.label) + "\",\"id\":\"" + win::jsonEscape(info.deviceId) + "\",\"msg\":\"" + win::jsonEscape(info.status.message) + "\"}";
    }
    j += "]";
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
    std::wstring screenshotPath, listCamerasPath;
    {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        for (int i = 1; argv && i + 1 < argc; ++i) {
            if (std::wstring(argv[i]) == L"--screenshot") screenshotPath = argv[i + 1];
            if (std::wstring(argv[i]) == L"--list-cameras") listCamerasPath = argv[i + 1];
        }
        if (argv) LocalFree(argv);
    }
    if (!listCamerasPath.empty()) {
        // Diagnostic mode (also used by CI): print the detected video devices as JSON and exit.
        const auto devs = win::enumerateVideoDevices();
        std::string j = "[";
        for (std::size_t i = 0; i < devs.size(); ++i)
            j += std::string(i ? "," : "") + "{\"name\":\"" + win::jsonEscape(devs[i].name) + "\",\"virtual\":" +
                 (devs[i].virtualCam ? "true" : "false") + "}";
        j += "]";
        HANDLE f = CreateFileW(listCamerasPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            DWORD w = 0;
            WriteFile(f, j.data(), static_cast<DWORD>(j.size()), &w, nullptr);
            CloseHandle(f);
        }
        return 0;
    }

    win::trace("wWinMain start");
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    app::Engine engine;
    engine.start();
    win::trace("engine started");
    win::VirtualCameraController vcam(engine.program(), engine.logger());
    win::trace("vcam controller ready");

    std::wstring dataDir;
    {
        PWSTR p = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p))) {
            dataDir = std::wstring(p) + L"\\HBStudio\\WebView2";
            CoTaskMemFree(p);
        }
    }

    win::trace("settings loading");
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
    // ---- input devices (webcams, OBS Virtual Camera, ...) ----
    app::SourceAssignments assignments;
    std::string assignPath;
    {
        PWSTR p = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p))) {
            assignPath = win::narrow(std::wstring(p) + L"\\HBStudio\\inputs.txt");
            CoTaskMemFree(p);
        }
        if (!assignPath.empty()) assignments.load(assignPath);
    }
    std::mutex devMutex;
    std::map<int, std::shared_ptr<win::DShowSource>> devices;   // slot -> running capture (keeps it alive)
    std::map<std::string, std::string> knownNames;          // id -> friendly name from the last scan
    std::atomic<bool> scanning{false};
    // Declared AFTER everything its jobs touch: destroyed first, so queued jobs finish while those still exist.
    win::TaskQueue tasks;                                   // slow jobs: enumeration, closing capture graphs

    auto retire = [&](std::shared_ptr<win::DShowSource> d) {
        if (d) tasks.post([d] { d->stop(); });              // never block UI/Program thread on a driver
    };
    auto attach = [&](int slot, const std::string& id, const std::string& name) {
        auto src = std::make_shared<win::DShowSource>(id, name);
        src->start();
        std::shared_ptr<win::DShowSource> old;
        {
            std::lock_guard lock(devMutex);
            old = devices[slot];
            devices[slot] = src;
        }
        engine.assignSlot(slot, src, id, name);
        retire(old);
    };
    auto detach = [&](int slot) {
        std::shared_ptr<win::DShowSource> old;
        {
            std::lock_guard lock(devMutex);
            auto it = devices.find(slot);
            if (it != devices.end()) { old = it->second; devices.erase(it); }
        }
        engine.clearSlot(slot);
        retire(old);
    };
    auto scan = [&] {
        if (scanning.exchange(true)) return;
        tasks.post([&] {
            const auto devs = win::enumerateVideoDevices();
            std::string j = "{\"t\":\"devices\",\"busy\":false,\"list\":[";
            {
                std::lock_guard lock(devMutex);
                knownNames.clear();
                for (std::size_t i = 0; i < devs.size(); ++i) {
                    knownNames[devs[i].id] = devs[i].name;
                    j += std::string(i ? "," : "") + "{\"id\":\"" + win::jsonEscape(devs[i].id) + "\",\"name\":\"" +
                         win::jsonEscape(devs[i].name) + "\",\"virt\":" + (devs[i].virtualCam ? "true" : "false") + "}";
                }
            }
            j += "]}";
            host.postQueued("devices", j);
            scanning = false;
        });
    };

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
        const std::size_t maxParts = c0 == "ptzname" ? 4 : (c0 == "ptzcfg" ? 3 : (c0 == "ptzall" ? 2 : (c0 == "devassign" ? 3 : 8)));
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
        else if (c == "dissolve") engine.switcher().startDissolve(std::clamp(idx(1) > 0 ? idx(1) : 500, 50, 10000));
        else if (c == "vcam" && a.size() > 1) {
            if (a[1] == "activate") vcam.activate();
            else if (a[1] == "deactivate") vcam.deactivate();
        } else if (c == "devrefresh") {
            scan();
        } else if (c == "devassign" && a.size() >= 3) {
            const int slot = idx(1);
            std::string name = a[2];
            {
                std::lock_guard lock(devMutex);
                auto it = knownNames.find(a[2]);
                if (it != knownNames.end()) name = it->second;
            }
            if (slot >= 0 && slot < app::Engine::kSourceCount && assignments.set(slot, a[2], name)) {
                attach(slot, a[2], name);
                if (!assignPath.empty()) assignments.save(assignPath);
                toast("CAM " + std::to_string(slot + 1) + ": " + name + " conectando…");
            }
        } else if (c == "devclear") {
            const int slot = idx(1);
            if (slot >= 0 && slot < app::Engine::kSourceCount) {
                detach(slot);
                assignments.clear(slot);
                if (!assignPath.empty()) assignments.save(assignPath);
                toast("CAM " + std::to_string(slot + 1) + ": voltou para a fonte de teste.");
            }
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

    win::trace("before host.create");
    win::WebViewHost::Options opt;
    opt.userDataDir = dataDir;
    opt.title = L"HB Studio";

    std::thread feeder;
    auto startFeeder = [&] {
        if (feeder.joinable()) return;   // never start a second feeder (page reload)
        // Restore the inputs used last time and list the devices for the CÂMERAS tab.
        for (int i = 0; i < app::Engine::kSourceCount; ++i) {
            const auto e = assignments.get(i);
            if (!e.empty()) attach(i, e.id, e.name);
        }
        scan();
        feeder = std::thread([&] {
            win::trace("feeder thread start");
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

    win::trace("entering message loop");
    int rc;
    if (shot) {
        MSG msg;
        bool captured = false;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            if (msg.message == WM_APP + 2 && !captured) {
                captured = true;
                win::trace("capture trigger received");
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

    win::trace("message loop ended");
    running = false;
    {
        std::lock_guard lock(devMutex);
        for (auto& kv : devices) kv.second->stop();   // close capture graphs before the engine goes away
    }
    if (feeder.joinable()) feeder.join();
    win::trace("feeder joined");
    vcam.shutdown();
    win::trace("vcam shut down");
    engine.stop();
    win::trace("engine stopped");
    CoUninitialize();
    return rc;
}
