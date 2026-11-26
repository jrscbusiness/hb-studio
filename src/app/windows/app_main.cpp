// HBStudio.exe - main application: native window + WebView2 UI + engine.
//
//   HBStudio.exe                    normal start
//   HBStudio.exe --screenshot X.png renders the UI, saves a PNG and exits (CI / support)
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <commdlg.h>
#include <shlobj.h>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "app/command_router.hpp"
#include "app/engine.hpp"
#include "app/state_json.hpp"
#include "app/media_service.hpp"
#include "app/ptz_settings.hpp"
#include "app/source_assignments.hpp"
#include "app/stream_destinations.hpp"
// ---- FEATURE: ai ----
#include "ai/ai_commands.hpp"
#include "ai/secret_store.hpp"
// ---- /FEATURE ----
#include "app/windows/dshow_capture.hpp"
#include "app/windows/stream_sender.hpp"
#include "app/windows/task_queue.hpp"
#include "app/windows/resource_ids.h"
#include "app/windows/vcam_controller.hpp"
#include "app/windows/webview_host.hpp"
#include "app/windows/wic_jpeg.hpp"
#include "app/windows/win_util.hpp"
#include "app/windows/output_window.hpp"
#include "ndi/ndi.hpp"
#include "scene/scene_service.hpp"   // FEATURE: scenes
#include "marketplace/service.hpp"   // FEATURE: marketplace
// ---- FEATURE: diagnostics ----
#include "diagnostics/checklist.hpp"
#include "diagnostics/report.hpp"
#include "diagnostics/settings.hpp"
#include "diagnostics/windows/sys_win.hpp"
// ---- /FEATURE: diagnostics ----
// ---- FEATURE: cloudbackup ----
#include "cloud/cloud_platform.hpp"
#include "cloud/cloud_service.hpp"
// ---- /FEATURE: cloudbackup ----
// ---- FEATURE: chroma ----
#include "chroma/service.hpp"
#include "chroma/windows/winhttp_client.hpp"
#include "chroma/windows/wic_image.hpp"
#ifdef HB_HAVE_ONNX
#include "chroma/onnx/onnx_segmenter.hpp"
#endif
// ---- /FEATURE: chroma ----
#include "video/frame_ops.hpp"
#include "plugins/service.hpp"
// ---- FEATURE: remote ----
#include <functional>
#include "remote/service.hpp"
#include "video/frame_ops.hpp"
// ---- /FEATURE ----
#include "vst3/vst3.hpp"
// ---- FEATURE: teleprompter ----
#include <filesystem>
#include "app/windows/presenter_window.hpp"
#include "teleprompter/controller.hpp"
#include "teleprompter/presenter_page.hpp"
// ---- FEATURE: overlays ----
#include "overlay/manager.hpp"
#include "app/template_overlays.hpp"   // FEATURE: templates->overlays
#include "overlay/windows/gdi_glyphs.hpp"
// ---- /FEATURE ----
#include "chat/service.hpp"   // FEATURE: chat
// ---- FEATURE: captions ----
#include "captions/controller.hpp"
#include "captions/windows/winhttp_client.hpp"
#ifdef HB_HAVE_WHISPER
#include "captions/whisper_recognizer.hpp"
#endif
// ---- /FEATURE ----

using namespace hb;

namespace {

// Gathers the live objects into a plain snapshot; the JSON layout itself lives in src/app/state_json.cpp.
std::string stateJson(app::Engine& engine, win::VirtualCameraController& vcam, const app::SourceAssignments& assignments,
                      int toastId, const std::string& toast) {
    const auto s = engine.snapshot();
    const auto v = vcam.status();
    const auto pf = engine.program().format();
    app::StateSnapshot st;
    st.version = HB_VERSION;
    st.pvw = s.sw.pvw;
    st.pgm = s.sw.pgm;
    st.transitioning = s.sw.transitioning;
    st.progress = s.sw.progress;
    st.fps = s.fps;
    st.targetFps = s.targetFps;
    st.programWidth = pf.width;
    st.programHeight = pf.height;
    st.frames = s.produced;
    st.skipped = s.skipped;
    st.tier = s.tier;
    st.cores = s.cores;
    st.ramGiB = s.ramGiB;
    st.sourcesRecommended = s.sourcesRecommended;
    st.sourcesMax = s.sourcesMax;
    st.vcam.supported = v.supported;
    st.vcam.installed = v.installed;
    st.vcam.active = v.active;
    st.vcam.connected = v.connected;
    st.vcam.busy = v.busy;
    st.vcam.error = v.error;
    st.vcam.sent = v.sent;
    st.vcam.message = v.message;
    st.toastId = toastId;
    st.toast = toast;
    for (int i = 0; i < app::Engine::kSourceCount; ++i) {
        const auto info = engine.slotInfo(i);
        const auto assigned = assignments.get(i);
        // A disabled slot has been detach()'d (engine.slotInfo reports not live, same as an empty
        // slot), so the remembered device (id/name from SourceAssignments) must be overlaid here -
        // otherwise turning a camera off would look identical to never having assigned one.
        app::StateSlot sl;
        sl.enabled = assigned.empty() || assigned.enabled;
        if (info.live) {
            using S = video::SourceStatus::State;
            sl.kind = "dev";
            sl.status = info.status.state == S::Online ? "online" : info.status.state == S::Connecting ? "connecting"
                      : info.status.state == S::NoSignal ? "nosignal" : "error";
            sl.label = info.label;
            sl.deviceId = info.deviceId;
            sl.message = info.status.message;
        } else if (!assigned.empty() && !sl.enabled) {
            sl.kind = "dev";
            sl.status = "off";
            sl.label = assigned.name;
            sl.deviceId = assigned.id;
        }
        sl.width = info.status.width;
        sl.height = info.status.height;
        sl.fps = info.status.fps;
        st.slots.push_back(std::move(sl));
    }
    auto tail = engine.logTail();
    // 200, not the sidebar widget's old 9: the CONFIG tab's full Logs panel needs real history to
    // be worth copying into a bug report; the small PRODUÇÃO sidebar widget still only shows its
    // own last few lines (sliced client-side), so this bump doesn't change that widget's look.
    const std::size_t from = tail.size() > 200 ? tail.size() - 200 : 0;
    for (std::size_t i = from; i < tail.size(); ++i) {
        const auto& r = tail[i];
        const std::time_t t = std::chrono::system_clock::to_time_t(r.time);
        std::tm tm{};
        localtime_s(&tm, &t);
        char line[512];
        snprintf(line, sizeof line, "%02d:%02d:%02d [%s] [%s] %s", tm.tm_hour, tm.tm_min, tm.tm_sec,
                 core::levelName(r.level), r.module.c_str(), r.message.c_str());
        st.log.push_back(app::StateLogLine{line, core::levelName(r.level)});
    }
    return app::buildStateJson(st);
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

    // ---- Program output format: a saved choice from CONFIG > Formatos de saída comuns, read
    // before the engine exists since ProgramOutput's resolution/fps is fixed for its whole
    // lifetime (changing it live would mean resizing every consumer - switcher sources, the
    // virtual camera, recording, NDI/RTMP output - while they run; out of scope for now). A
    // changed format only takes effect the next time HB Studio starts, which "outfmt"'s toast says.
    video::ProgramFormat outputFormat{1920, 1080, {30, 1}};
    {
        const std::wstring localAppData = win::localAppDataPath();
        if (!localAppData.empty()) {
            std::ifstream f(win::narrow(localAppData + L"\\HBStudio\\outputformat.txt"), std::ios::binary);
            int w = 0, h = 0, fn = 0, fd = 0;
            if (f && (f >> w >> h >> fn >> fd) && w > 0 && h > 0 && fn > 0 && fd > 0 && w <= 7680 && h <= 4320)
                outputFormat = {w, h, {fn, fd}};
        }
    }

    app::Engine engine(outputFormat);
    engine.start();
    win::trace("engine started");
    win::VirtualCameraController vcam(engine.program(), engine.logger());
    win::trace("vcam controller ready");

    std::wstring dataDir;
    {
        const std::wstring localAppData = win::localAppDataPath();
        if (!localAppData.empty()) dataDir = localAppData + L"\\HBStudio\\WebView2";
    }

    win::trace("settings loading");
    win::WebViewHost host;
    std::atomic<bool> running{true};
    // ---- PTZ settings (names + per-camera configuration), persisted next to the WebView2 data ----
    app::PtzSettings ptzSettings;
    std::string ptzPath;
    {
        const std::wstring localAppData = win::localAppDataPath();
        if (!localAppData.empty()) {
            const std::wstring dir = localAppData + L"\\HBStudio";
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
        const std::wstring localAppData = win::localAppDataPath();
        if (!localAppData.empty()) assignPath = win::narrow(localAppData + L"\\HBStudio\\inputs.txt");
        if (!assignPath.empty()) assignments.load(assignPath);
    }
    // ---- TRANSMISSÃO: outbound RTMP(S) destinations, persisted next to the other settings ----
    app::StreamDestinations destinations;
    std::string destPath;
    {
        const std::wstring localAppData = win::localAppDataPath();
        if (!localAppData.empty()) destPath = win::narrow(localAppData + L"\\HBStudio\\xmit.json");
        if (!destPath.empty()) destinations.load(destPath);
    }
    std::mutex sendersMutex;
    std::map<std::string, std::unique_ptr<win::StreamSender>> senders;   // destination id -> running sender
    // ---- output format (CONFIG > Formatos de saída comuns): same file read at the top into
    // `outputFormat`, kept here too so the "outfmt" command can save a new choice for next launch.
    std::string outFmtPath;
    {
        const std::wstring localAppData = win::localAppDataPath();
        if (!localAppData.empty()) outFmtPath = win::narrow(localAppData + L"\\HBStudio\\outputformat.txt");
    }
    // ---- plugins (themes + native video-source plugins), installed per user ----
    std::string pluginsDir;
    {
        const std::wstring localAppData = win::localAppDataPath();
        if (!localAppData.empty()) pluginsDir = win::narrow(localAppData + L"\\HBStudio\\plugins");
    }
    plugins::PluginService plugins(pluginsDir, "windows-x64");
    std::atomic<bool> pluginsReady{false};
    // ---- audio mixer + recording ----
    std::string mediaDir, videosDir;
    {
        const std::wstring localAppData = win::localAppDataPath();
        if (!localAppData.empty()) mediaDir = win::narrow(localAppData + L"\\HBStudio");
        PWSTR v = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Videos, 0, nullptr, &v))) {
            videosDir = win::narrow(std::wstring(v) + L"\\HB Studio");
            CoTaskMemFree(v);
        }
    }
    app::MediaService media(engine.program(), &engine.bus(), &engine.logger(), mediaDir, videosDir);
    media.start();
    // ---- FEATURE: multitrack ----
    // Cameras are sampled CLEAN from their slots (before overlays/transitions); the app only describes them here.
    {
        app::IsoService::Providers iso;
        iso.cameras = [&engine] {
            std::vector<app::IsoCamera> v;
            for (int i = 0; i < app::Engine::kSourceCount; ++i) {
                const auto info = engine.slotInfo(i);
                app::IsoCamera c;
                c.slot = i;
                c.live = info.live;
                c.label = "CAM " + std::to_string(i + 1);
                c.device = info.label;
                c.width = info.status.width;
                c.height = info.status.height;
                c.fps = info.status.fps;
                v.push_back(c);
            }
            return v;
        };
        iso.source = [&engine](int slot) { return engine.slotSource(slot); };
        media.iso().setProviders(std::move(iso));
    }
    // ---- /FEATURE ----
    win::trace("media service started");

    // ---- FEATURE: overlays ---- timer / scoreboard / QR / lower third / captions / chat pin over PROGRAM
    // (docs/OVERLAYS.md). Declared after `engine`, so it is destroyed first; the render hook only keeps the
    // shared render state alive, never the manager or the bus.
    const std::string overlaysPath = mediaDir.empty() ? std::string() : mediaDir + "\\overlays.json";
    hb::overlay::OverlayManager overlays(&engine.bus(), nullptr, hb::overlay::makeGdiTextRasterizer());
    if (!overlaysPath.empty() && !overlays.load(overlaysPath)) engine.logger().info("overlay", "sem overlays salvos");
    engine.setPostRenderHook(overlays.programHook());
    std::string overlayLastState;
    std::mutex overlayMutex;   // guards overlayLastState (UI message thread vs feeder thread)
    auto postOverlayState = [&](bool force) {
        const std::string st = overlays.stateJson();
        if (!force && st == overlayLastState) return;
        overlayLastState = st;
        host.postQueued("overlay_state", st);
    };
    // ---- /FEATURE ----

    // ---- native dark/light mode (built into the app; no plugin needed) ----
    const std::string themePath = mediaDir.empty() ? std::string() : mediaDir + "\\theme.txt";
    auto effectiveDark = [](const std::string& pref) {
        if (pref == "dark" || pref == "gray" || pref == "obs") return true;  // "gray"/"obs" are dark palettes too
        if (pref == "light") return false;
        return win::systemPrefersDark();  // "system" (default)
    };
    std::string themePref = "system";
    if (!themePath.empty()) {
        std::ifstream f(themePath, std::ios::binary);
        std::string v;
        if (f && std::getline(f, v) && (v == "dark" || v == "light" || v == "gray" || v == "obs" || v == "system")) themePref = v;
    }

    // ---- CONFIG > Qualidade do Preview: unlike outputFormat above, this applies live (no
    // restart) since it only changes the size of the small JPEG preview frames, not the real
    // Program Output - so it's safe to just set it straight on the already-constructed engine.
    const std::string previewQPath = mediaDir.empty() ? std::string() : mediaDir + "\\previewquality.txt";
    if (!previewQPath.empty()) {
        std::ifstream f(previewQPath, std::ios::binary);
        std::string v;
        if (f && std::getline(f, v)) engine.setPreviewQuality(v);   // unknown/corrupt id: keeps the default
    }

    std::mutex devMutex;
    std::map<int, std::shared_ptr<win::DShowSource>> devices;   // slot -> running capture (keeps it alive)
    std::map<int, std::shared_ptr<hb::ndi::NdiSource>> ndiDevices;  // slot -> running NDI receiver
    std::map<std::string, std::string> knownNames;          // id -> friendly name from the last scan
    hb::ndi::NdiDiscovery ndiDiscovery;                      // lists NDI sources currently on the network
    // ---- outputs: second/third monitor windows + NDI output of PROGRAM ----
    win::OutputWindow progWin(L"HB Studio — PROGRAM"), prevWin(L"HB Studio — PREVIEW");
    std::unique_ptr<hb::ndi::NdiSender> ndiSender;  // created only when the operator turns NDI output on
    std::mutex ndiSenderMutex;
    bool ndiOutputEnabled = false;
    std::string ndiOutputName = "HB Studio Program";
    const std::string ndiOutPath = mediaDir.empty() ? std::string() : mediaDir + "\\ndiout.txt";
    if (!ndiOutPath.empty()) {
        std::ifstream f(ndiOutPath, std::ios::binary);
        std::string enabledLine, nameLine;
        if (f && std::getline(f, enabledLine)) {
            ndiOutputEnabled = enabledLine == "1";
            if (std::getline(f, nameLine) && !nameLine.empty() && nameLine.size() <= 128) ndiOutputName = nameLine;
        }
    }
    auto saveNdiOutSettings = [&] {
        if (ndiOutPath.empty()) return;
        std::ofstream f(ndiOutPath, std::ios::binary | std::ios::trunc);
        f << (ndiOutputEnabled ? "1" : "0") << "\n" << ndiOutputName;
    };
    auto postOutputsState = [&] {
        bool ndiAvailable;
        {
            std::lock_guard lock(ndiSenderMutex);
            ndiAvailable = ndiSender ? ndiSender->available() : true;  // "true": unknown yet, don't warn early
        }
        host.postQueued("outputs", std::string("{\"t\":\"outputs\",\"ndiEnabled\":") + (ndiOutputEnabled ? "true" : "false") +
                                        ",\"ndiName\":\"" + win::jsonEscape(ndiOutputName) + "\",\"ndiAvailable\":" +
                                        (ndiAvailable ? "true" : "false") + "}");
    };
    // The destination list itself (names/servers/settings) only changes on an explicit edit, so it's
    // only pushed then + on "ready"; per-destination live status is cheap and safe to push often
    // (the feeder tick does, at the same ~4x/s cadence as "rec" - see media.tick() below).
    auto postXmitDest = [&] { host.postQueued("xmit_dest", destinations.uiJson()); };
    // The SAVED (pending) output format choice, which may differ from the one actually running
    // (engine.program().format(), already sent in every "s" message as pw/ph/target) until the
    // next restart - see the big comment where `outputFormat` is read, above.
    auto postOutFmt = [&] {
        const auto pf = engine.program().format();
        host.postQueued("outfmt", "{\"t\":\"outfmt\",\"w\":" + std::to_string(outputFormat.width) + ",\"h\":" +
                                       std::to_string(outputFormat.height) + ",\"fpsNum\":" + std::to_string(outputFormat.fps.num) +
                                       ",\"fpsDen\":" + std::to_string(outputFormat.fps.den) + ",\"activeW\":" +
                                       std::to_string(pf.width) + ",\"activeH\":" + std::to_string(pf.height) + ",\"activeFpsNum\":" +
                                       std::to_string(pf.fps.num) + ",\"activeFpsDen\":" + std::to_string(pf.fps.den) + "}");
    };
    auto postXmitStatus = [&] {
        std::string j = "{\"t\":\"xmit_status\",\"list\":[";
        {
            std::lock_guard lock(sendersMutex);
            bool first = true;
            for (const auto& kv : senders) {
                const auto st = kv.second->status();
                j += std::string(first ? "" : ",") + "{\"id\":\"" + win::jsonEscape(kv.first) + "\",\"running\":" +
                     (st.running ? "true" : "false") + ",\"publishing\":" + (st.publishing ? "true" : "false") +
                     ",\"message\":\"" + win::jsonEscape(st.message) + "\",\"videoUnits\":" + std::to_string(st.videoUnits) +
                     ",\"audioUnits\":" + std::to_string(st.audioUnits) + "}";
                first = false;
            }
        }
        j += "]}";
        host.postQueued("xmit_status", j);
    };
    // ---- FEATURE: cloudbackup ---- automatic recording backup to S3-compatible storage / Google Drive.
    // Uploads run on one low-priority background thread, never while a live stream runs (default),
    // credentials are DPAPI-protected (cloud_secrets.json) and never sent to the UI.
    std::shared_ptr<hb::cloud::ISecretStore> cloudSecrets;
    if (!mediaDir.empty()) cloudSecrets = hb::cloud::makePlatformSecretStore(mediaDir + "\\cloud_secrets.json");
    if (!cloudSecrets) cloudSecrets = std::make_shared<hb::cloud::MemorySecretStore>();
    hb::cloud::CloudServiceDeps cloudDeps;
    cloudDeps.dataDir = mediaDir;
    cloudDeps.http = hb::cloud::makePlatformHttp();
    cloudDeps.secrets = cloudSecrets;
    cloudDeps.fs = hb::cloud::makePlatformFileSystem();
    cloudDeps.bus = &engine.bus();
    cloudDeps.recordingsFolder = [&] { return media.settings().folder; };
    cloudDeps.streamingActive = [&] {
        std::lock_guard lock(sendersMutex);
        for (const auto& kv : senders)
            if (kv.second->status().running) return true;
        return false;
    };
    cloudDeps.openBrowser = [](const std::string& url) { ShellExecuteW(nullptr, L"open", win::widen(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL); };
    cloudDeps.threadInit = [] { hb::cloud::setBackgroundThreadPriority(); };
    hb::cloud::CloudService cloudSvc(cloudDeps);
    cloudSvc.start();
    std::mutex cloudMutex;
    std::string cloudLast;
    auto postCloud = [&](bool force) {
        const std::string j = cloudSvc.stateJson();
        std::lock_guard lock(cloudMutex);
        if (force || j != cloudLast) { cloudLast = j; host.postQueued("cloud_state", j); }
    };
    // ---- /FEATURE: cloudbackup ----
    std::atomic<bool> scanning{false};
    // Declared AFTER everything its jobs touch: destroyed first, so queued jobs finish while those still exist.
    win::TaskQueue tasks;                                   // slow jobs: enumeration, closing capture graphs

    auto retire = [&](std::shared_ptr<win::DShowSource> d) {
        if (d) tasks.post([d] { d->stop(); });              // never block UI/Program thread on a driver
    };
    auto retireNdi = [&](std::shared_ptr<hb::ndi::NdiSource> d) {
        if (d) tasks.post([d] { d->stop(); });
    };
    // "ndi:<endereço>" -> NdiSourceInfo (reconstructed; the endereço is opaque, only meaningful to
    // the NDI backend, and is exactly what defaultBackend()'s findCurrentSources() returned).
    auto parseNdiId = [](const std::string& id) { return id.rfind("ndi:", 0) == 0 ? id.substr(4) : std::string(); };
    auto attachNdi = [&](int slot, const std::string& id, const std::string& name) {
        hb::ndi::NdiSourceInfo info;
        info.name = name;
        info.urlAddress = parseNdiId(id);
        auto src = std::make_shared<hb::ndi::NdiSource>(info);
        src->start();
        std::shared_ptr<hb::ndi::NdiSource> old;
        {
            std::lock_guard lock(devMutex);
            old = ndiDevices[slot];
            ndiDevices[slot] = src;
        }
        engine.assignSlot(slot, src, id, name);
        retireNdi(old);
    };
    auto attachPlugin = [&](int slot, const std::string& id, const std::string& name, std::string& err) {
        auto src = plugins.runtime().open(id, err);
        if (!src) return false;
        std::shared_ptr<win::DShowSource> old;
        {
            std::lock_guard lock(devMutex);
            auto it = devices.find(slot);
            if (it != devices.end()) { old = it->second; devices.erase(it); }
        }
        engine.assignSlot(slot, src, id, name);
        retire(old);
        return true;
    };
    auto attach = [&](int slot, const std::string& id, const std::string& name, win::VideoFormat fmt = {}) {
        auto src = std::make_shared<win::DShowSource>(id, name, fmt);
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
        std::shared_ptr<hb::ndi::NdiSource> oldNdi;
        {
            std::lock_guard lock(devMutex);
            auto it = devices.find(slot);
            if (it != devices.end()) { old = it->second; devices.erase(it); }
            auto itN = ndiDevices.find(slot);
            if (itN != ndiDevices.end()) { oldNdi = itN->second; ndiDevices.erase(itN); }
        }
        engine.clearSlot(slot);
        retire(old);
        retireNdi(oldNdi);
    };
    auto scan = [&] {
        if (scanning.exchange(true)) return;
        tasks.post([&] {
            const auto devs = win::enumerateVideoDevices();
            std::string j = "{\"t\":\"devices\",\"busy\":false,\"list\":[";
            bool firstDev = true;
            {
                std::lock_guard lock(devMutex);
                knownNames.clear();
                for (std::size_t i = 0; i < devs.size(); ++i) {
                    knownNames[devs[i].id] = devs[i].name;
                    firstDev = false;
                    j += std::string(i ? "," : "") + "{\"id\":\"" + win::jsonEscape(devs[i].id) + "\",\"name\":\"" +
                         win::jsonEscape(devs[i].name) + "\",\"virt\":" + (devs[i].virtualCam ? "true" : "false") + "}";
                }
            }
            if (pluginsReady) {
                std::lock_guard lock(devMutex);
                for (const auto& ps : plugins.runtime().sources()) {
                    knownNames[ps.fullId] = ps.name;
                    j += std::string(firstDev ? "" : ",") + "{\"id\":\"" + win::jsonEscape(ps.fullId) + "\",\"name\":\"" + win::jsonEscape(ps.name) +
                         "\",\"virt\":false,\"plugin\":\"" + win::jsonEscape(ps.pluginName) + "\"}";
                    firstDev = false;
                }
            }
            {
                std::lock_guard lock(devMutex);
                for (const auto& ns : ndiDiscovery.sources()) {
                    const std::string id = "ndi:" + ns.urlAddress;
                    knownNames[id] = ns.name;
                    j += std::string(firstDev ? "" : ",") + "{\"id\":\"" + win::jsonEscape(id) + "\",\"name\":\"" +
                         win::jsonEscape(ns.name) + "\",\"virt\":false,\"ndi\":true}";
                    firstDev = false;
                }
            }
            j += "],\"ndiMsg\":\"" + win::jsonEscape(ndiDiscovery.available() ? "" : ndiDiscovery.unavailableMessage()) + "\"}";
            host.postQueued("devices", j);
            scanning = false;
        });
    };
    // Starts (or restarts) one destination's sender; stopXmit tears it down. Both are id-keyed so a
    // destination's own failure/restart never touches the others (the per-destination isolation the
    // module's requirements call for).
    auto startXmit = [&](const std::string& id) {
        const auto d = destinations.get(id);
        if (d.empty()) return;
        std::lock_guard lock(sendersMutex);
        auto& slot = senders[id];
        if (!slot) slot = std::make_unique<win::StreamSender>(d, engine.program(), media.audio());
        slot->start();
    };
    auto stopXmit = [&](const std::string& id) {
        std::unique_ptr<win::StreamSender> doomed;
        {
            std::lock_guard lock(sendersMutex);
            auto it = senders.find(id);
            if (it == senders.end()) return;
            doomed = std::move(it->second);
            senders.erase(it);
        }
        // std::function (what TaskQueue::post takes) requires a copy-constructible target, so a
        // move-only unique_ptr can't be captured directly - share ownership into the one task instead.
        std::shared_ptr<win::StreamSender> shared = std::move(doomed);
        tasks.post([shared]() { shared->stop(); });   // stop() joins a thread: never on the UI thread
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
    // ---- FEATURE: chat ---- unified live chat (docs/CHAT.md): YouTube / Facebook / Instagram(limited) / Twitch.
    // Declared after `engine` and `host`, so it is destroyed first; its threads are also stopped explicitly
    // at shutdown. Secrets are DPAPI-encrypted per user under %LOCALAPPDATA%\HBStudio\chat_secrets.
    auto chatHttp = hb::chat::makeDefaultChatHttp();
    auto chatSocket = hb::chat::makeDefaultChatSocket();
    auto chatSecrets = mediaDir.empty() ? nullptr : hb::chat::makeDpapiSecretStore(mediaDir + "\\chat_secrets");
    hb::chat::ChatService chatSvc([&] {
        hb::chat::ChatDeps d;
        d.bus = &engine.bus();
        d.http = chatHttp.get();
        d.socket = chatSocket.get();
        d.secrets = chatSecrets.get();
        if (!mediaDir.empty()) { d.settingsPath = mediaDir + "\\chat.json"; d.logPath = mediaDir + "\\chat_log.jsonl"; }
        d.post = [&host](const std::string& key, std::string json) { host.postQueued(key, std::move(json)); };
        d.toast = toast;
        // Only the Google consent page we built ourselves is ever opened.
        d.openUrl = [](const std::string& url) {
            if (url.rfind("https://accounts.google.com/", 0) != 0) return false;
            return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", win::widen(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
        };
        return d;
    }());
    chatSvc.start();
    // ---- /FEATURE ----
    // ---- FEATURE: ai ----
    // AI providers (CONFIG > Inteligência Artificial). Declared after `host` so worker threads are joined
    // (AiService destructor) before the window goes away. Keys: DPAPI-encrypted file, never sent to the UI.
    std::unique_ptr<hb::ai::ISecretStore> aiSecrets = hb::ai::makeDefaultSecretStore();
    hb::ai::ProviderStore aiStore(aiSecrets.get());
    std::string aiPath;
    {
        const std::wstring localAppData = win::localAppDataPath();
        if (!localAppData.empty()) aiPath = win::narrow(localAppData + L"\\HBStudio\\ai.json");
        if (!aiPath.empty()) aiStore.load(aiPath);
    }
    hb::ai::AiService aiService(aiStore, hb::ai::AiClient(hb::ai::AiClientDeps{hb::ai::makeDefaultAiHttp(), {}, {}}));
    // ---- /FEATURE ----
    // ---- FEATURE: ai ----
    hb::ai::AiCommands aiCommands(aiService, aiStore, aiPath, [&](const std::string& key, std::string json) { host.postQueued(key, std::move(json)); },
                                  [&](const std::string& t) { toast(t); });
    // ---- /FEATURE ----
    auto ptzChanged = [&] {
        if (!ptzPath.empty() && !ptzSettings.save(ptzPath)) engine.logger().warning("ptz", "não foi possível salvar a configuração");
        ptzSent = 0;  // force the feeder to resend
    };
    constexpr const char* kNoPtz = "nenhuma câmera PTZ conectada (disponível nas Fases 4–5). Nada foi enviado.";

    // "cmd|a|b|rest" splitting lives in app/command_router.hpp (hb::app::splitCommand, unit-tested).
    auto split = [](const std::string& msg, std::size_t maxParts) { return app::splitCommand(msg, maxParts); };

    // ---- FEATURE: remote ----
    // Phone/tablet remote control (src/remote). Remote sessions reach THIS SAME handler (onMessage)
    // after the server's strict command allowlist rebuilt the text, so no logic is duplicated.
    // OFF until the operator enables it in CONFIG; commands arrive on the server's executor thread
    // (the allowlisted commands only touch thread-safe parts: switcher, media, senders).
    std::function<void(const std::string&)> remoteDispatch;
    hb::remote::RemoteService remoteSvc(
        [&](const std::string& cmd) { if (remoteDispatch) remoteDispatch(cmd); },
        [&](const std::string& line) { engine.logger().info("remote", line); });
    // ---- FEATURE: scenes ---- CENAS bank (scenes.json in %LOCALAPPDATA%\HBStudio)
    scene::SceneService sceneSvc(mediaDir.empty() ? std::string() : mediaDir + "\\scenes.json",
                                 [&](int slot, std::shared_ptr<const scene::Scene> s) { engine.setScene(slot, std::move(s)); });
    sceneSvc.load();
    auto postScenes = [&] { host.postQueued("scenes", sceneSvc.stateJson()); };
    // ---- /FEATURE ----

    // ---- FEATURE: marketplace ---- community catalog (static index.json over HTTPS), download + SHA-256 check,
    // then the EXISTING plugin review/consent dialog (plugins.beginInstall) - never bypassed.
    const std::string mktDir = mediaDir.empty() ? std::string() : mediaDir + "\\marketplace";
    const std::string mktUrlFile = mktDir.empty() ? std::string() : mktDir + "\\catalog_url.txt";
    auto mktDownloader = marketplace::makeDefaultDownloader();
    marketplace::MarketplaceConfig mktCfg;
    mktCfg.cacheDirUtf8 = mktDir.empty() ? std::string() : mktDir + "\\cache";
    mktCfg.downloadDirUtf8 = mktDir.empty() ? std::string() : mktDir + "\\tmp";
    mktCfg.appVersion = HB_VERSION;
    mktCfg.platform = "windows-x64";
    {
        std::ifstream f(mktUrlFile, std::ios::binary);
        std::string u;
        if (f && std::getline(f, u)) {
            while (!u.empty() && (u.back() == '\r' || u.back() == ' ')) u.pop_back();
            if (marketplace::isSafeHttpsUrl(u)) mktCfg.catalogUrl = u;   // a bad file just falls back to the default
        }
    }
    marketplace::PluginHooks mktHooks;
    mktHooks.installedVersions = [&] {
        std::map<std::string, std::string> m;
        for (const auto& p : plugins.manager().list()) m[p.manifest.id] = p.manifest.version;
        return m;
    };
    mktHooks.beginInstall = [&](const std::string& path) { return plugins.beginInstall(path); };
    mktHooks.cancelInstall = [&](const std::string& token) { plugins.cancelInstall(token); };
    mktHooks.importTemplate = [&](const std::string& path) {
        const auto r = sceneSvc.handle("sceneimport|" + path);
        if (r.changed) postScenes();
        return std::make_pair(r.changed, r.message.empty() ? std::string("Template importado.") : r.message);
    };
    marketplace::MarketplaceService marketSvc(*mktDownloader, mktCfg, mktHooks);
    // Declared after everything its jobs touch: destroyed first. A separate queue so a long download never
    // delays device enumeration on `tasks`.
    win::TaskQueue marketTasks;
    auto postMarket = [&] { host.postQueued("mkt", marketSvc.catalogJson()); };
    // ---- /FEATURE ----

    // ---- FEATURE: teleprompter ---- host-authoritative scroll state + script library
    // (%LOCALAPPDATA%\HBStudio\teleprompter). Commands "tp|..." arrive from the main UI, from the
    // presenter window and (play/pause/speed/seek) from the remote allowlist; see docs/TELEPROMPTER.md.
    hb::teleprompter::SteadyClockSource tpClock;
    hb::teleprompter::Controller tpc(std::filesystem::path(win::widen(mediaDir) + L"\\teleprompter"), &tpClock);
    tpc.load();
    std::function<void(const std::string&)> tpHandle;
    win::PresenterWindow tpPresenter(
        [&](const std::string& m) { if (m.rfind("tp|", 0) == 0 && tpHandle) tpHandle(m); },
        [&](bool open) { tpc.setPresenterOpen(open); host.postQueued("tp", tpc.stateJson()); });
    auto tpBroadcast = [&](const char* key, const std::string& json) {
        host.postQueued(key, json);
        tpPresenter.broadcast(key, json);
    };
    tpHandle = [&](const std::string& msg) {
        const auto fx = tpc.execute(msg);
        if (!fx.toast.empty()) toast(fx.toast);
        using P = hb::teleprompter::Effects::Presenter;
        if (fx.presenter == P::Open) {
            tpPresenter.open(inst, dataDir, win::widen(mediaDir) + L"\\teleprompter-window.txt",
                             hb::teleprompter::extractPresenterPage(win::loadResourceText(IDR_UI_HTML)),
                             [&] { return std::vector<std::string>{tpc.styleJson(), tpc.scriptJson(), tpc.stateJson()}; });
        } else if (fx.presenter == P::Close) {
            tpPresenter.close();
        } else if (fx.presenter == P::Fullscreen) {
            tpPresenter.toggleFullscreen();
        }
        if (fx.list) host.postQueued("tplist", tpc.listJson());
        if (fx.script) tpBroadcast("tpscript", tpc.scriptJson());
        if (fx.style) tpBroadcast("tpstyle", tpc.styleJson());
        if (fx.state) tpBroadcast("tp", tpc.stateJson());
    };
    // ---- /FEATURE ----
    // ---- FEATURE: captions ---- Legendas ao vivo: local whisper.cpp (or optional cloud) -> "caption.text" on the bus
    // (docs/LEGENDAS.md). Taps the Program audio through a bounded drop-oldest queue on its own worker thread.
    hb::captions::win::WinHttpClient captionHttp;
    hb::captions::ControllerDeps captionDeps;
    captionDeps.bus = &engine.bus();
    captionDeps.settingsPath = mediaDir.empty() ? std::string() : mediaDir + "\\captions.json";
    captionDeps.modelsDir = mediaDir.empty() ? std::string() : mediaDir + "\\models";
    captionDeps.srtFolder = [&] { const std::string f = media.settings().folder; return f.empty() ? videosDir : f; };
    captionDeps.downloader = &captionHttp;
    captionDeps.http = &captionHttp;
    captionDeps.addProgramConsumer = [&](std::function<void(const float*, std::size_t)> c) {
        return media.audio().addProgramConsumer(std::move(c));
    };
    captionDeps.removeProgramConsumer = [&](std::uint64_t id) { media.audio().removeProgramConsumer(id); };
#ifdef HB_HAVE_WHISPER
    captionDeps.makeWhisper = [](const std::string& path) -> std::unique_ptr<hb::captions::ISpeechRecognizer> {
        return std::make_unique<hb::captions::WhisperRecognizer>(path);
    };
#endif
    hb::captions::CaptionController captions(std::move(captionDeps));
    // ---- /FEATURE ----
    // ---- FEATURE: chroma ---- per-camera background removal (classic chroma key + local AI)
    chroma::WinHttpClient chromaHttp;
    chroma::ChromaService::Deps chromaDeps;
    chromaDeps.modelsDir = mediaDir.empty() ? std::string() : mediaDir + "\\models";
    chromaDeps.http = &chromaHttp;
#ifdef HB_HAVE_ONNX
    chromaDeps.segmenterFactory = [](const std::string& path, std::string& err) { return chroma::createOnnxSegmenter(path, err); };
#endif
    chromaDeps.imageLoader = [](const std::string& path, video::Frame& out, std::string& err) { return chroma::loadImageWic(path, out, err); };
    chromaDeps.onChanged = [&](int slot, const std::string& ser) {
        // Persisted in inputs.txt's optional 9th field; only slots with an assigned device are saved.
        if (assignments.setBackground(slot, ser) && !assignPath.empty()) assignments.save(assignPath);
    };
    chroma::ChromaService chromaSvc(app::Engine::kSourceCount, std::move(chromaDeps));
    for (int i = 0; i < app::Engine::kSourceCount; ++i) {
        engine.setSlotFilter(i, chromaSvc.filter(i));
        chromaSvc.load(i, assignments.get(i).bg);
    }
    auto postChroma = [&] { host.postQueued("chroma", chromaSvc.statusJson()); };
    // ---- /FEATURE: chroma ----

    // ---- Command router (app/command_router.hpp): simple commands that only call one service. The big
    // if/else chain in onMessage below still handles everything not registered here. Names registered
    // here must not be claimed by an earlier branch of that chain (tp, cloud*, cap, ai*, chroma*, chat*, ov*, ready).
    app::CommandRouter router;
    router.add("preview", [&](const app::Command& c) { engine.switcher().setPreview(c.num(1)); });
    router.add("program", [&](const app::Command& c) { engine.switcher().setProgram(c.num(1)); });
    router.add("cut", [&](const app::Command&) { engine.switcher().cut(); });
    router.add("dissolve", [&](const app::Command& c) {
        engine.switcher().startDissolve(std::clamp(c.num(1) > 0 ? c.num(1) : 500, 50, 10000));
    });
    router.add("tbar", [&](const app::Command& c) {
        engine.switcher().setTBarPosition(static_cast<float>(std::atof(c.a[1].c_str())));
    }, 2);
    router.add("vcam", [&](const app::Command& c) {
        if (c.a[1] == "activate") vcam.activate();
        else if (c.a[1] == "deactivate") vcam.deactivate();
    }, 2);
    router.add("devrefresh", [&](const app::Command&) { scan(); });
    router.add("previewq", [&](const app::Command& c) {
        if (engine.setPreviewQuality(c.a[1])) {
            if (!previewQPath.empty()) {
                std::ofstream f(previewQPath, std::ios::binary | std::ios::trunc);
                f << c.a[1];
            }
            host.postQueued("previewq", "{\"t\":\"previewq\",\"id\":\"" + win::jsonEscape(c.a[1]) + "\"}");
        }
    }, 2);
    router.add("plugincancel", [&](const app::Command& c) { plugins.cancelInstall(c.a[1]); }, 2);
    router.add("uitheme", [&](const app::Command& c) {
        if (!(c.a[1] == "light" || c.a[1] == "dark" || c.a[1] == "gray" || c.a[1] == "obs" || c.a[1] == "system")) return;
        // Native dark/light mode: built into the app, no plugin needed. The web page applies
        // its own colours (CSS + localStorage, see ui/index.html); this just keeps the native
        // window chrome (title bar) in sync and remembers the choice for the next launch.
        themePref = c.a[1];
        host.setTheme(effectiveDark(themePref));
        if (!themePath.empty()) {
            std::ofstream f(themePath, std::ios::binary | std::ios::trunc);
            f << themePref;
        }
    }, 2);
    router.add("outwin", [&](const app::Command& c) {
        // Second/third monitor output: a plain window the operator drags to another screen and
        // maximizes/fullscreens (F11) there - see src/app/windows/output_window.hpp.
        win::OutputWindow& win_ = c.a[1] == "preview" ? prevWin : progWin;
        if (c.a[2] == "open") win_.open(inst);
        else if (c.a[2] == "close") win_.close();
    }, 3);
    router.add("ptzname", [&](const app::Command& c) {
        if (ptzSettings.setPresetName(c.num(1), c.num(2), c.a[3])) ptzChanged();
    }, 4, 4);
    router.add("ptzcfg", [&](const app::Command& c) {
        app::PtzCameraConfig cfg;
        if (app::PtzSettings::parseConfig(c.a[2], cfg) && ptzSettings.setConfig(c.num(1), cfg)) {
            ptzChanged();
            toast("CAM " + std::to_string(c.num(1) + 1) + ": " + "configuração PTZ guardada.");
        }
    }, 3, 3);
    router.add("ptzall", [&](const app::Command& c) {
        app::PtzCameraConfig cfg;
        if (app::PtzSettings::parseConfig(c.a[1], cfg)) {
            ptzSettings.applyToAll(cfg);
            ptzChanged();
            toast("Configuração PTZ aplicada a todas as câmeras.");
        }
    }, 2, 2);
    for (const char* ptzNoop : {"ptzgo", "ptzsave", "ptzmove"})
        router.add(ptzNoop, [&](const app::Command& c) {
            // No PTZ provider is connected yet (Phases 4-5): say so instead of pretending.
            toast("CAM " + std::to_string(c.num(1) + 1) + ": " + kNoPtz);
        });
    // ptzstop / ptzzoom / ptzfocus: silent no-ops until a provider exists.
    router.add("xmitsave", [&](const app::Command& c) {
        app::StreamDestination d;
        if (app::StreamDestinations::parseDestination(c.a[1], d)) {
            // The UI never receives a saved key back in full (see StreamDestinations::uiJson),
            // so an edit that didn't touch the key arrives with it blank - keep the stored one.
            if (!d.id.empty() && d.streamKey.empty()) {
                const auto existing = destinations.get(d.id);
                if (!existing.empty()) d.streamKey = existing.streamKey;
            }
            const std::string savedId = destinations.upsert(d);
            if (!savedId.empty()) {
                if (!destPath.empty()) destinations.save(destPath);
                postXmitDest();
                toast(d.name.empty() ? "Destino de transmissão salvo." : ("Destino \"" + d.name + "\" salvo."));
            } else {
                toast("Não foi possível salvar: nome/servidor inválido ou limite de 16 destinos atingido.");
            }
        }
    }, 2, 2);
    router.add("xmitremove", [&](const app::Command& c) {
        stopXmit(c.a[1]);
        if (destinations.remove(c.a[1])) {
            if (!destPath.empty()) destinations.save(destPath);
            postXmitDest();
            postXmitStatus();
        }
    }, 2);
    router.add("xmitenable", [&](const app::Command& c) {
        if (destinations.setEnabled(c.a[1], c.a[2] == "1")) {
            if (!destPath.empty()) destinations.save(destPath);
            if (c.a[2] != "1") stopXmit(c.a[1]);   // turning a destination off also stops it if running
            postXmitDest();
            postXmitStatus();
        }
    }, 3);
    router.add("xmitstart", [&](const app::Command& c) {
        startXmit(c.a[1]);
        postXmitStatus();
    }, 2);
    router.add("xmitstop", [&](const app::Command& c) {
        stopXmit(c.a[1]);
        postXmitStatus();
    }, 2);
    router.add("xmitstartall", [&](const app::Command&) {
        for (const auto& d : destinations.list())
            if (d.enabled) startXmit(d.id);
        postXmitStatus();
    });
    router.add("xmitstopall", [&](const app::Command&) {
        for (const auto& d : destinations.list()) stopXmit(d.id);
        postXmitStatus();
    });
    // ---- FEATURE: remote ----
    router.add("remote", [&](const app::Command& c) {   // desktop UI only: remote|on / off / newpin / revoke / kick|id
        const std::string t = remoteSvc.handleUi(c.raw);
        if (!t.empty()) toast(t);
        host.postQueued("remote", remoteSvc.statusJson());
    });
    // ---- /FEATURE ----

    // ---- FEATURE: diagnostics ---- "Exportar diagnóstico" + "Checklist antes de entrar ao vivo" (docs/DIAGNOSTICO.md).
    // Portable logic: src/diagnostics (report + redactor + checks); Windows probes: diagnostics/windows/sys_win.cpp.
    hb::diag::ChecklistSettings chkSettings;
    const std::string chkPath = mediaDir.empty() ? std::string() : mediaDir + "\\checklist.json";
    if (!chkPath.empty()) {
        std::ifstream f(chkPath, std::ios::binary);
        if (f) {
            std::string text(16 * 1024 + 1, '\0');
            f.read(text.data(), static_cast<std::streamsize>(text.size()));
            text.resize(static_cast<std::size_t>(f.gcount()));
            chkSettings = hb::diag::ChecklistSettings::fromJson(text);
        }
    }
    auto postChkCfg = [&] { host.postQueued("chkcfg", chkSettings.uiJson()); };
    win::TaskQueue diagTasks;                 // own thread: the checklist may wait seconds on the network
    std::atomic<bool> chkBusy{false};

    // Everything the checklist knows without probing, gathered on the UI thread.
    auto gatherChecklistInput = [&] {
        hb::diag::ChecklistInput in;
        const auto rs = media.settings();
        in.recordingFolder = rs.folder.empty() ? videosDir : rs.folder;
        in.recordingVideoKbps = rs.videoBitrateKbps;
        in.recordingAudioKbps = rs.audioBitrateKbps;
        in.minFreeBytes = static_cast<std::int64_t>(rs.minFreeMegabytes) * 1024 * 1024;
        for (const auto& d : destinations.list()) {
            hb::diag::ChecklistDestination cd;
            cd.name = d.name;
            cd.enabled = d.enabled;
            cd.urlSet = !d.serverUrl.empty();
            cd.keySet = !d.streamKey.empty();   // only the boolean ever leaves this function
            cd.videoKbps = d.videoKbps;
            cd.audioKbps = d.audioKbps;
            in.destinations.push_back(std::move(cd));
        }
        for (int i = 0; i < app::Engine::kSourceCount; ++i) {
            const auto as = assignments.get(i);
            if (as.empty()) continue;
            const auto info = engine.slotInfo(i);
            hb::diag::ChecklistCamera cam;
            cam.slot = i;
            cam.name = as.name;
            cam.enabled = as.enabled;
            cam.online = info.live && info.status.state == video::SourceStatus::State::Online;
            cam.detail = info.status.message;
            in.cameras.push_back(std::move(cam));
        }
        for (const auto& st : media.audio().snapshot().strips) {
            hb::diag::ChecklistAudioStrip as;
            as.index = st.index;
            as.name = st.name;
            as.assigned = st.inputKind != audio_io::InputKind::None;
            as.state = st.inputState;
            as.peakDb = std::max({static_cast<double>(st.peakDb[0]), static_cast<double>(st.peakDb[1]),
                                  static_cast<double>(st.holdDb[0]), static_cast<double>(st.holdDb[1])});
            in.audio.push_back(std::move(as));
        }
        in.speedTestEnabled = chkSettings.speedTestEnabled;
        in.speedTestUrl = chkSettings.speedTestUrl;
        return in;
    };

    // Redactor: every saved stream key verbatim + the user's folders as %ENV% aliases.
    auto makeRedactor = [&] {
        hb::diag::Redactor r;
        for (const auto& d : destinations.list()) r.addSecret(d.streamKey);
        const std::wstring lad = win::localAppDataPath();
        if (!lad.empty()) r.addPathAlias(win::narrow(lad), "%LOCALAPPDATA%");
        PWSTR prof = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Profile, 0, nullptr, &prof))) {
            r.addPathAlias(win::narrow(prof), "%USERPROFILE%");
            CoTaskMemFree(prof);
        }
        return r;
    };

    auto gatherDiagnostics = [&] {
        hb::diag::DiagnosticsInput d;
        d.appVersion = HB_VERSION;
        const auto snap = engine.snapshot();
        d.logicalCores = snap.cores;
        d.ramGiB = snap.ramGiB;
        const auto pf = engine.program().format();
        d.outputFormat = std::to_string(pf.width) + "x" + std::to_string(pf.height) + " @ " + std::to_string(pf.fps.num) +
                         (pf.fps.den != 1 ? "/" + std::to_string(pf.fps.den) : std::string()) + " fps";
        d.previewQuality = engine.previewQuality();
        // video devices from the last scan, with the slot that uses them
        {
            std::lock_guard lock(devMutex);
            for (const auto& kv : knownNames) {
                hb::diag::DeviceLine dl;
                dl.name = kv.second;
                dl.status = "disponível";
                for (int i = 0; i < app::Engine::kSourceCount; ++i) {
                    const auto as = assignments.get(i);
                    if (as.empty() || as.id != kv.first) continue;
                    const auto info = engine.slotInfo(i);
                    static const char* const kState[] = {"conectando", "online", "sem sinal", "erro"};
                    dl.status = "CAM " + std::to_string(i + 1) + (as.enabled ? "" : " (desligada)");
                    dl.detail = std::string(info.live ? kState[static_cast<int>(info.status.state)] : "offline") +
                                (info.status.width > 0 ? ", " + std::to_string(info.status.width) + "x" + std::to_string(info.status.height) : std::string());
                    if (!info.status.message.empty()) dl.detail += ", " + info.status.message;
                    if (info.status.state == video::SourceStatus::State::Error || info.status.state == video::SourceStatus::State::NoSignal)
                        d.extraErrors.push_back({"CAM " + std::to_string(i + 1), info.status.message.empty() ? dl.detail : info.status.message, ""});
                    break;
                }
                d.videoDevices.push_back(std::move(dl));
            }
        }
        // assigned cameras whose device is missing from the last scan
        for (int i = 0; i < app::Engine::kSourceCount; ++i) {
            const auto as = assignments.get(i);
            if (as.empty()) continue;
            bool listed = false;
            {
                std::lock_guard lock(devMutex);
                listed = knownNames.count(as.id) != 0;
            }
            if (!listed) d.videoDevices.push_back({as.name, "CAM " + std::to_string(i + 1), "dispositivo ausente na última busca"});
        }
        const auto aud = media.audio().snapshot();
        for (const auto& dev : aud.devices)
            d.audioDevices.push_back({dev.name, std::string(dev.kind == audio_io::DeviceKind::Capture ? "entrada" : "saída") + (dev.isDefault ? " (padrão)" : ""),
                                      std::to_string(dev.channels) + " canais, " + std::to_string(dev.sampleRate) + " Hz"});
        for (const auto& st : aud.strips)
            if (st.inputKind != audio_io::InputKind::None)
                d.audioDevices.push_back({"Canal " + std::to_string(st.index + 1) + " (" + st.name + ") <- " + st.inputName, st.inputState, st.inputMessage});
        // features
        const auto vs = vcam.status();
        d.features.push_back({"Saída NDI", ndiOutputEnabled ? "ligado" : "desligado", false});
        d.features.push_back({"Câmera virtual", vs.installed ? (vs.active ? "ativa" : "instalada") : "não instalada", false});
        d.features.push_back({"Controle remoto (celular)", remoteSvc.enabled() ? "ligado" : "desligado", false});
        d.features.push_back({"Gravando agora", media.recording() ? "sim" : "não", false});
        d.features.push_back({"Janela PROGRAM aberta", progWin.isOpen() ? "sim" : "não", false});
        d.features.push_back({"Janela PREVIEW aberta", prevWin.isOpen() ? "sim" : "não", false});
        // settings (no secrets)
        const auto rs = media.settings();
        d.settings.push_back({"Perfil de gravação", rs.profileId, false});
        d.settings.push_back({"Pasta de gravação", rs.folder.empty() ? videosDir : rs.folder, false});
        d.settings.push_back({"Bitrate de gravação (vídeo/áudio kbps)", std::to_string(rs.videoBitrateKbps) + "/" + std::to_string(rs.audioBitrateKbps), false});
        d.settings.push_back({"Áudio de gravação", std::to_string(rs.audioSampleRate) + " Hz, " + std::to_string(rs.audioBitsPerSample) + " bits", false});
        d.settings.push_back({"Limite mínimo de espaço livre (MB)", std::to_string(rs.minFreeMegabytes), false});
        d.settings.push_back({"Tema", themePref, false});
        d.settings.push_back({"Perfil do computador (tier)", snap.tier, false});
        d.settings.push_back({"Quadros produzidos / perdidos", std::to_string(snap.produced) + " / " + std::to_string(snap.skipped), false});
        d.settings.push_back({"Teste de upload do checklist", chkSettings.speedTestEnabled ? "ligado" : "desligado", false});
        auto clip = [](std::string t) { if (t.size() > 1500) t.resize(1500); return t; };
        d.settings.push_back({"Estado do chat (UI)", clip(chatSvc.statusJson()), false});
        d.settings.push_back({"Estado do backup em nuvem (UI)", clip(cloudSvc.stateJson()), false});
        // stream destinations: never the key, only whether it is set; sender status when running
        for (const auto& dst : destinations.list()) {
            hb::diag::DestinationLine dl;
            dl.name = dst.name;
            dl.platform = dst.platform;
            dl.protocol = dst.protocol;
            dl.serverUrl = dst.serverUrl;
            dl.enabled = dst.enabled;
            dl.urlSet = !dst.serverUrl.empty();
            dl.keySet = !dst.streamKey.empty();
            dl.videoKbps = dst.videoKbps;
            dl.audioKbps = dst.audioKbps;
            {
                std::lock_guard lock(sendersMutex);
                auto it = senders.find(dst.id);
                if (it != senders.end() && it->second) {
                    const auto st = it->second->status();
                    dl.status = std::string(st.publishing ? "transmitindo" : (st.running ? "conectando" : "parado")) + ": " + st.message;
                    if (!st.publishing && !st.message.empty()) d.extraErrors.push_back({"TRANSMISSÃO " + dst.name, st.message, ""});
                }
            }
            d.destinations.push_back(std::move(dl));
        }
        d.log = engine.logTail();
        return d;
    };

    router.add("diagexport", [&](const app::Command&) {
        // The destination path comes ONLY from the native Save dialog: nothing in the message is used as a path.
        wchar_t file[MAX_PATH * 4] = {0};
        OPENFILENAMEW ofn{};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = host.hwnd();
        ofn.lpstrFile = file;
        ofn.nMaxFile = static_cast<DWORD>(sizeof(file) / sizeof(file[0]));
        ofn.lpstrFilter = L"Diagnóstico (*.txt)\0*.txt\0";
        ofn.lpstrDefExt = L"txt";
        ofn.lpstrTitle = L"Exportar diagnóstico";
        ofn.Flags = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT | OFN_EXPLORER | OFN_NOCHANGEDIR | OFN_HIDEREADONLY;
        wcsncpy_s(file, L"HBStudio-diagnostico.txt", _TRUNCATE);
        if (!GetSaveFileNameW(&ofn)) return;
        const std::string path = win::narrow(file);
        auto input = gatherDiagnostics();
        auto redactor = makeRedactor();
        diagTasks.post([&toast, input = std::move(input), redactor = std::move(redactor), path]() mutable {
            const auto si = hb::diag::sys::querySystemInfo();
            input.windowsVersion = si.windowsVersion;
            input.cpuName = si.cpuName;
            input.gpus = si.gpus;
            input.monitors = si.monitors;
            const std::string text = hb::diag::buildReport(input, redactor);
            std::string err;
            if (hb::diag::writeTextFile(path, text, err)) toast("Diagnóstico salvo. Anexe o arquivo ao relato do problema.");
            else toast("Não foi possível salvar o diagnóstico: " + err);
        });
    });
    router.add("chkrun", [&](const app::Command&) {
        if (chkBusy.exchange(true)) return;
        host.postQueued("checklist", "{\"t\":\"checklist\",\"busy\":true}");
        auto input = gatherChecklistInput();
        diagTasks.post([&host, &chkBusy, input = std::move(input)]() mutable {
            std::string json;
            try {
                json = hb::diag::checklistJson(hb::diag::runChecklist(input, hb::diag::sys::makeProbes()));
            } catch (...) {
                json = "{\"t\":\"checklist\",\"error\":true,\"results\":[]}";
            }
            host.postQueued("checklist", json);
            chkBusy = false;
        });
    });
    router.add("chkcfg", [&](const app::Command& c) {   // chkcfg|0/1|https://endpoint
        const std::string url = c.str(2);
        const bool want = c.str(1) == "1";
        if (!url.empty() && !hb::diag::isValidSpeedTestUrl(url)) {
            toast("Endereço do teste inválido: use um link https:// completo, sem usuário/senha.");
            postChkCfg();
            return;
        }
        chkSettings.speedTestUrl = url;
        chkSettings.speedTestEnabled = want && !url.empty();
        if (want && url.empty()) toast("Informe o endereço HTTPS do teste de upload para ativá-lo.");
        if (!chkPath.empty()) {
            std::ofstream f(chkPath, std::ios::binary | std::ios::trunc);
            f << chkSettings.toJson();
        }
        postChkCfg();
    }, 2, 3);
    // ---- /FEATURE: diagnostics ----

    auto onMessage = [&](const std::string& msg) {
        if (router.dispatch(msg)) return;
        const std::string c0 = msg.substr(0, msg.find('|'));
        if (c0 == "tp") { tpHandle(msg); return; }   // FEATURE: teleprompter
        // ---- FEATURE: cloudbackup ----
        if (c0.rfind("cloud", 0) == 0) {
            const std::string t = cloudSvc.handle(split(msg, c0 == "cloudsave" ? 2 : 3));
            if (!t.empty()) toast(t);
            postCloud(true);
            return;
        }
        // ---- /FEATURE: cloudbackup ----
        if (c0 == "cap") { captions.handleCommand(msg); host.postQueued("cap", captions.pollJson(true)); return; }   // FEATURE: captions
        // (ptz*, xmit* and the other router-registered commands carry their own limits in router.add)
        const std::size_t maxParts = c0 == "devassign" ? 3 : (c0 == "devformats" ? 2 : 8);
        auto a = split(msg, maxParts);
        if (a.empty()) return;
        auto idx = [&](std::size_t k) { return k < a.size() ? std::atoi(a[k].c_str()) : -1; };
        const std::string& c = a[0];
        // ---- FEATURE: ai ----
        if (c0.rfind("ai", 0) == 0 && aiCommands.handle(msg)) return;
        // ---- /FEATURE ----
        // ---- FEATURE: chroma ----
        if (c == "chroma" && a.size() >= 4) {
            std::string m;
            const int slot = idx(1);
            if (chromaSvc.handleSetting(slot, a[2], a[3], m) && !m.empty()) toast("CAM " + std::to_string(slot + 1) + ": " + m);
            postChroma();
            return;
        }
        if (c == "chromamodel" && a.size() >= 2) {
            std::string m;
            chromaSvc.handleModel(a[1], m);
            if (!m.empty()) toast(m);
            postChroma();
            return;
        }
        if (c == "chromaimg" && a.size() >= 2) {
            // Native file dialog (UI thread): the chosen image becomes this slot's replacement background.
            const int slot = idx(1);
            wchar_t file[MAX_PATH * 4] = {0};
            OPENFILENAMEW ofn{};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = host.hwnd();
            ofn.lpstrFilter = L"Imagens (*.png;*.jpg;*.jpeg;*.bmp)\0*.png;*.jpg;*.jpeg;*.bmp\0Todos os arquivos\0*.*\0";
            ofn.lpstrFile = file;
            ofn.nMaxFile = static_cast<DWORD>(sizeof(file) / sizeof(file[0]));
            ofn.lpstrTitle = L"Escolha a imagem de fundo";
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_NOCHANGEDIR | OFN_HIDEREADONLY;
            if (GetOpenFileNameW(&ofn)) {
                std::string m;
                chromaSvc.handleSetting(slot, "repl", "image", m);
                chromaSvc.handleSetting(slot, "img", win::narrow(file), m);
                postChroma();
            }
            return;
        }
        // ---- /FEATURE: chroma ----
        // ---- FEATURE: overlays ----
        // ---- FEATURE: chat ----
        if (c0 == "ready") chatSvc.postAll();
        if (c0 == "ready") postChkCfg();   // FEATURE: diagnostics
        else if (c0.rfind("chat", 0) == 0 && chatSvc.handleCommand(msg)) return;
        // ---- /FEATURE ----
        if (c0 == "ready") { std::lock_guard lk(overlayMutex); postOverlayState(true); }
        else if (c0.rfind("ov", 0) == 0 && overlays.handleCommand(msg)) {
            std::lock_guard lk(overlayMutex);
            if (overlays.takeDirty() && !overlaysPath.empty()) overlays.save(overlaysPath);
            postOverlayState(true);
            return;
        }
        // ---- /FEATURE ----
        if (c == "ready") {
            ptzSent = 0;
            media.forceResend();
            if (pluginsReady) {
                host.postQueued("plugins", plugins.listJson());
                host.postQueued("theme", plugins.themeJson());
            }
            postOutputsState();
            postXmitDest();
            postXmitStatus();
            postOutFmt();
            postScenes();   // FEATURE: scenes
            postChroma();   // FEATURE: chroma
            postCloud(true);   // FEATURE: cloudbackup
            host.postQueued("cap", captions.pollJson(true));   // FEATURE: captions
            host.postQueued("previewq", "{\"t\":\"previewq\",\"id\":\"" + win::jsonEscape(engine.previewQuality()) + "\"}");
        }
        else if (c == "ndiout" && a.size() >= 2) {
            if (a[1] == "on" || a[1] == "off") {
                ndiOutputEnabled = a[1] == "on";
                if (a.size() >= 3 && !a[2].empty() && a[2].size() <= 128) ndiOutputName = a[2];
                std::lock_guard lock(ndiSenderMutex);
                if (ndiOutputEnabled) {
                    if (!ndiSender) ndiSender = std::make_unique<hb::ndi::NdiSender>(ndiOutputName);
                } else {
                    ndiSender.reset();
                }
                saveNdiOutSettings();
                const std::string msg = !ndiOutputEnabled ? "Saída NDI desligada."
                                       : (ndiSender->available() ? "Saída NDI ligada: \"" + ndiOutputName + "\" na rede."
                                                                  : "Sem o NDI Runtime instalado, a saída NDI fica indisponível.");
                toast(msg);
                postOutputsState();
            }
        }
        else if (c == "aud" || c == "rec") {
            if (c == "rec" && a.size() >= 2 && a[1] == "pickfolder") {
                // Native folder dialog (UI thread).
                BROWSEINFOW bi{};
                bi.hwndOwner = host.hwnd();
                bi.lpszTitle = L"Escolha a pasta das gravações";
                bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_USENEWUI;
                if (PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi)) {
                    wchar_t path[MAX_PATH * 2] = {0};
                    if (SHGetPathFromIDListW(pidl, path)) media.setFolder(win::narrow(path));
                    CoTaskMemFree(pidl);
                }
            }
            // ---- FEATURE: multitrack ----
            else if (c == "rec" && a.size() >= 3 && a[1] == "iso" && a[2] == "open") {
                // Opens the last multitrack session folder (or the recording folder) in Explorer.
                const std::wstring dir = win::widen(media.isoFolder());
                if (!dir.empty()) ShellExecuteW(host.hwnd(), L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            }
            // ---- /FEATURE ----
            else {
                const std::string t = media.handle(msg);
                if (!t.empty()) toast(t);
            }
        }
        else if (c == "fxeditor" && a.size() >= 4) {
            // Opens/closes a VST3 effect's own native GUI editor window. Any other effect type
            // (built-in, native plugin) just doesn't implement IVst3Editor, so the dynamic_cast
            // below is null and this is a harmless, silent no-op for it.
            const int strip = idx(1);
            int effIdx = -1;
            if (a.size() > 2) effIdx = std::atoi(a[2].c_str());
            auto fx = media.audio().effectInstance(strip, effIdx);
            auto* editor = dynamic_cast<vst3::IVst3Editor*>(fx.get());
            if (!editor) {
                // Not a VST3 effect (or the strip/idx no longer points at one): silent no-op.
            } else if (a[3] == "open") {
                if (!editor->hasEditor()) {
                    toast("Este plugin VST3 não tem interface gráfica própria.");
                } else {
                    std::string err;
                    const std::string title = "CANAL " + std::to_string(strip + 1) + " · efeito " + std::to_string(effIdx + 1);
                    if (!editor->openEditor(title, err)) toast("Não foi possível abrir o editor: " + err);
                }
            } else if (a[3] == "close") {
                editor->closeEditor();
            }
        }
        // ---- FEATURE: marketplace ----
        else if (c.rfind("mkt", 0) == 0) {   // mktlist | mktrefresh | mktinstall|id | mktcancel | mktcfg|url
            if (c == "mktlist") {
                postMarket();
            } else if (c == "mktrefresh") {
                marketTasks.post([&] {
                    marketSvc.refresh();
                    postMarket();
                });
            } else if (c == "mktinstall" && a.size() >= 2) {
                const std::string id = a[1];
                if (marketSvc.installing()) {
                    toast("Já existe uma instalação em andamento.");
                } else {
                    marketTasks.post([&, id] {
                        postMarket();   // shows "installing"
                        const auto out = marketSvc.install(id, [&, id](const std::string& phase, int pct) {
                            host.postQueued("mkt_progress", "{\"t\":\"mkt_progress\",\"id\":\"" + win::jsonEscape(id) + "\",\"phase\":\"" +
                                                                phase + "\",\"pct\":" + std::to_string(pct) + "}");
                        });
                        if (!out.contextJson.empty()) host.postQueued("", out.contextJson);
                        if (!out.reviewJson.empty()) host.postQueued("", out.reviewJson);   // opens the existing consent dialog
                        if (!out.ok || out.imported)
                            host.postQueued("", "{\"t\":\"mkt_done\",\"id\":\"" + win::jsonEscape(id) + "\",\"ok\":" + (out.ok ? "true" : "false") +
                                                    ",\"message\":\"" + win::jsonEscape(out.message) + "\"}");
                        else
                            host.postQueued("", "{\"t\":\"mkt_done\",\"id\":\"" + win::jsonEscape(id) + "\",\"ok\":true,\"message\":\"\"}");
                        postMarket();
                    });
                }
            } else if (c == "mktcancel") {
                marketSvc.cancelInstall();
            } else if (c == "mktcfg") {
                const std::string url = a.size() >= 2 ? a[1] : std::string();
                if (marketSvc.setCatalogUrl(url)) {
                    if (!mktDir.empty()) {
                        std::error_code ec;
                        std::filesystem::create_directories(std::filesystem::path(std::u8string(mktDir.begin(), mktDir.end())), ec);
                        std::ofstream f(mktUrlFile, std::ios::binary | std::ios::trunc);
                        f << (url.empty() ? std::string() : url);   // empty file = default catalog
                    }
                    toast(url.empty() ? "Catálogo padrão restaurado." : "Catálogo do Marketplace alterado.");
                    marketTasks.post([&] {
                        marketSvc.refresh();
                        postMarket();
                    });
                } else {
                    toast("Endereço inválido: use um link https:// completo.");
                    postMarket();
                }
            }
        }
        // ---- /FEATURE ----
        // ---- FEATURE: scenes ----
        else if (sceneSvc.isSceneCommand(msg)) {
            std::string m = msg;
            wchar_t file[MAX_PATH * 4] = {0};
            OPENFILENAMEW ofn{};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = host.hwnd();
            ofn.lpstrFile = file;
            ofn.nMaxFile = static_cast<DWORD>(sizeof(file) / sizeof(file[0]));
            ofn.lpstrFilter = L"Templates do HB Studio (*.json)\0*.json\0Todos os arquivos\0*.*\0";
            ofn.lpstrDefExt = L"json";
            // Import/export ALWAYS go through the native file dialog: any path argument that arrived in the
            // message (e.g. from a compromised page) is ignored, so a script cannot read or overwrite
            // arbitrary files with scene-service permissions.
            if (c == "sceneimport") {
                ofn.lpstrTitle = L"Importar template";
                ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_NOCHANGEDIR | OFN_HIDEREADONLY;
                if (!GetOpenFileNameW(&ofn)) return;
                m = "sceneimport|" + win::narrow(file);
            } else if (c == "sceneexport") {
                if (a.size() < 2) return;
                ofn.lpstrTitle = L"Exportar template";
                ofn.Flags = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT | OFN_EXPLORER | OFN_NOCHANGEDIR | OFN_HIDEREADONLY;
                const std::wstring def = win::widen(a[1]) + L".json";
                wcsncpy_s(file, def.c_str(), _TRUNCATE);
                if (!GetSaveFileNameW(&ofn)) return;
                m = "sceneexport|" + a[1] + "|" + win::narrow(file);
            }
            const auto r = sceneSvc.handle(m);
            if (!r.message.empty()) toast(r.message);
            if (r.changed) postScenes();
            // FEATURE: templates->overlays: applying a template also creates its lower-third/QR/etc. as editable, off-air GRÁFICOS elements
            if (r.ok && c == "sceneapply") {
                std::lock_guard lk(overlayMutex);
                int made = 0;
                for (int s = 0; s < app::Engine::kSceneCount; ++s)
                    if (const auto sc = sceneSvc.sceneAt(s)) made += app::createOverlaysFromScene(overlays, *sc);
                if (made > 0) {
                    if (overlays.takeDirty() && !overlaysPath.empty()) overlays.save(overlaysPath);
                    postOverlayState(true);
                    toast("Gráficos do modelo criados na aba GRÁFICOS (fora do ar).");
                }
            }
        }
        // ---- /FEATURE ----
        else if (c == "devassign" && a.size() >= 3) {
            const int slot = idx(1);
            std::string name = a[2];
            {
                std::lock_guard lock(devMutex);
                auto it = knownNames.find(a[2]);
                if (it != knownNames.end()) name = it->second;
            }
            if (slot >= 0 && slot < app::Engine::kSourceCount && assignments.set(slot, a[2], name)) {
                const std::string id = a[2];
                if (id.rfind("plugin:", 0) == 0) {
                    tasks.post([&, slot, id, name] {
                        std::string err;
                        if (!attachPlugin(slot, id, name, err)) {
                            assignments.clear(slot);
                            toast("CAM " + std::to_string(slot + 1) + ": não foi possível abrir a fonte do plugin (" + err + ").");
                        } else {
                            toast("CAM " + std::to_string(slot + 1) + ": " + name);
                        }
                        if (!assignPath.empty()) assignments.save(assignPath);
                    });
                } else if (id.rfind("ndi:", 0) == 0) {
                    attachNdi(slot, id, name);
                    if (!assignPath.empty()) assignments.save(assignPath);
                    toast("CAM " + std::to_string(slot + 1) + ": " + name + " (NDI) conectando…");
                } else {
                    attach(slot, id, name);
                    if (!assignPath.empty()) assignments.save(assignPath);
                    toast("CAM " + std::to_string(slot + 1) + ": " + name + " conectando…");
                }
            }
        } else if (c == "devclear") {
            const int slot = idx(1);
            if (slot >= 0 && slot < app::Engine::kSourceCount) {
                chromaSvc.reset(slot);   // FEATURE: chroma - its saved background goes with the device
                detach(slot);
                assignments.clear(slot);
                if (!assignPath.empty()) assignments.save(assignPath);
                toast("CAM " + std::to_string(slot + 1) + ": voltou para a fonte de teste.");
            }
        } else if (c == "devenable" && a.size() >= 2) {
            // Re-attach the device this slot remembers, without touching which one it is - the
            // counterpart of "devdisable" below, not a re-assignment (that's "devassign").
            const int slot = idx(1);
            if (slot >= 0 && slot < app::Engine::kSourceCount) {
                const auto e = assignments.get(slot);
                if (!e.empty() && assignments.setEnabled(slot, true)) {
                    if (!assignPath.empty()) assignments.save(assignPath);
                    const std::string id = e.id, name = e.name;
                    const win::VideoFormat fmt{e.w, e.h, e.fpsNum, e.fpsDen};
                    if (id.rfind("plugin:", 0) == 0) {
                        tasks.post([&, slot, id, name] {
                            std::string err;
                            if (!attachPlugin(slot, id, name, err))
                                toast("CAM " + std::to_string(slot + 1) + ": não foi possível religar (" + err + ").");
                            else
                                toast("CAM " + std::to_string(slot + 1) + ": ligada.");
                        });
                    } else if (id.rfind("ndi:", 0) == 0) {
                        attachNdi(slot, id, name);
                        toast("CAM " + std::to_string(slot + 1) + ": ligada (NDI) conectando…");
                    } else {
                        attach(slot, id, name, fmt);
                        toast("CAM " + std::to_string(slot + 1) + ": ligada.");
                    }
                }
            }
        } else if (c == "devdisable" && a.size() >= 2) {
            // Stops capture but keeps the device remembered (unlike "devclear", which forgets it).
            const int slot = idx(1);
            if (slot >= 0 && slot < app::Engine::kSourceCount && assignments.setEnabled(slot, false)) {
                detach(slot);
                if (!assignPath.empty()) assignments.save(assignPath);
                toast("CAM " + std::to_string(slot + 1) + ": desligada.");
            }
        } else if (c == "devformats" && a.size() >= 2) {
            // Probes a device's supported resolutions/frame rates for the camera-settings dialog;
            // "id" is the device id as listed by devrefresh, not necessarily the slot's own device
            // (the dialog can be opened for any device shown in CÂMERAS).
            const std::string id = a[1];
            tasks.post([&, id] {
                std::string j = "{\"t\":\"devformats\",\"id\":\"" + win::jsonEscape(id) + "\",\"list\":[";
                bool first = true;
                for (const auto& f : win::enumerateDeviceFormats(id)) {
                    j += std::string(first ? "" : ",") + "{\"w\":" + std::to_string(f.width) + ",\"h\":" + std::to_string(f.height) +
                         ",\"fpsNum\":" + std::to_string(f.fpsNum) + ",\"fpsDen\":" + std::to_string(f.fpsDen) + "}";
                    first = false;
                }
                j += "]}";
                host.postQueued("", j);
            });
        } else if (c == "devformat" && a.size() >= 6) {
            // Remembers the chosen resolution/fps for this slot's device and, if it's currently
            // running, re-attaches it with the new format right away. NDI/plugin sources don't go
            // through DirectShow format negotiation, so the format is only ever applied to those.
            const int slot = idx(1);
            const int w = idx(2), h = idx(3), fn = idx(4), fd = idx(5);
            if (slot >= 0 && slot < app::Engine::kSourceCount && assignments.setFormat(slot, w, h, fn, fd)) {
                if (!assignPath.empty()) assignments.save(assignPath);
                const auto e = assignments.get(slot);
                if (e.enabled && !e.id.empty() && e.id.rfind("ndi:", 0) != 0 && e.id.rfind("plugin:", 0) != 0)
                    attach(slot, e.id, e.name, win::VideoFormat{e.w, e.h, e.fpsNum, e.fpsDen});
                toast("CAM " + std::to_string(slot + 1) + ": formato de captura atualizado.");
            }
        } else if (c == "outfmt" && a.size() >= 5) {
            // Only the 6 documented presets (CONFIG > Formatos de saída comuns) are accepted -
            // arbitrary resolutions aren't vetted against the switcher/virtual camera/recording
            // pipeline, all of which assume whatever ProgramOutput was constructed with.
            static const int kPresets[][4] = {{1280, 720, 30, 1}, {1280, 720, 60, 1}, {1920, 1080, 25, 1},
                                              {1920, 1080, 30, 1}, {1920, 1080, 60, 1}, {3840, 2160, 30, 1}};
            const int w = idx(1), h = idx(2), fn = idx(3), fd = idx(4);
            bool valid = false;
            for (const auto& p : kPresets)
                if (p[0] == w && p[1] == h && p[2] == fn && p[3] == fd) { valid = true; break; }
            if (valid) {
                outputFormat = {w, h, {fn, fd}};
                if (!outFmtPath.empty()) {
                    std::ofstream f(outFmtPath, std::ios::binary | std::ios::trunc);
                    f << w << ' ' << h << ' ' << fn << ' ' << fd;
                }
                postOutFmt();
                toast("Formato salvo: " + std::to_string(w) + "×" + std::to_string(h) + " · " + std::to_string(fn / fd) +
                     " fps. Reinicie o HB Studio para aplicar.");
            }
        } else if (c == "pluginpick") {
            // Native file dialog (UI thread). The chosen file is only INSPECTED; nothing is installed yet.
            wchar_t file[MAX_PATH * 4] = {0};
            OPENFILENAMEW ofn{};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = host.hwnd();
            ofn.lpstrFilter = L"Plugins do HB Studio (*.hbplugin;*.zip)\0*.hbplugin;*.zip\0Todos os arquivos\0*.*\0";
            ofn.lpstrFile = file;
            ofn.nMaxFile = static_cast<DWORD>(sizeof(file) / sizeof(file[0]));
            ofn.lpstrTitle = L"Escolha o plugin (.hbplugin)";
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_NOCHANGEDIR | OFN_HIDEREADONLY;
            if (GetOpenFileNameW(&ofn)) {
                const std::string path = win::narrow(file);
                tasks.post([&, path] { host.postQueued("", plugins.beginInstall(path)); });
            }
        } else if (c == "pluginconfirm" && a.size() >= 2) {
            const std::string token = a[1];
            tasks.post([&, token] {
                host.postQueued("", plugins.confirmInstall(token));
                host.postQueued("plugins", plugins.listJson());
            });
        } else if ((c == "pluginenable" || c == "pluginremove") && a.size() >= 2) {
            const std::string id = a[1];
            const bool enable = c == "pluginenable" && a.size() >= 3 && a[2] == "1";
            const bool removing = c == "pluginremove";
            tasks.post([&, id, enable, removing] {
                plugins::Result r = removing ? plugins.remove(id) : plugins.setEnabled(id, enable);
                if (!enable) {      // switched off or removed: inputs that came from this plugin go back to the test source
                    for (int i = 0; i < app::Engine::kSourceCount; ++i) {
                        const auto info = engine.slotInfo(i);
                        if (info.live && info.deviceId.rfind("plugin:" + id + ":", 0) == 0) {
                            detach(i);
                            assignments.clear(i);
                        }
                    }
                    if (!assignPath.empty()) assignments.save(assignPath);
                }
                media.syncPluginEffects(plugins);
                toast(r.message);
                host.postQueued("plugins", plugins.listJson());
                host.postQueued("theme", plugins.themeJson());
                scan();
            });
        }
    };

    // ---- FEATURE: remote ----
    remoteDispatch = [&](const std::string& cmd) { onMessage(cmd); };
    // ---- /FEATURE ----

    win::trace("before host.create");
    win::WebViewHost::Options opt;
    opt.userDataDir = dataDir;
    opt.title = L"HB Studio";
    if (!mediaDir.empty()) opt.placementFile = win::widen(mediaDir) + L"\\window.txt";
    opt.initialDark = effectiveDark(themePref);

    std::thread feeder;
    if (ndiOutputEnabled) {
        std::lock_guard lock(ndiSenderMutex);
        ndiSender = std::make_unique<hb::ndi::NdiSender>(ndiOutputName);
    }
    auto startFeeder = [&] {
        if (feeder.joinable()) return;   // never start a second feeder (page reload)
        // Restore the inputs used last time and list the devices for the CÂMERAS tab.
        for (int i = 0; i < app::Engine::kSourceCount; ++i) {
            const auto e = assignments.get(i);
            if (e.empty() || !e.enabled || e.id.rfind("plugin:", 0) == 0) continue;
            if (e.id.rfind("ndi:", 0) == 0) attachNdi(i, e.id, e.name);
            else attach(i, e.id, e.name, win::VideoFormat{e.w, e.h, e.fpsNum, e.fpsDen});
        }
        tasks.post([&] {   // plugins: scan the folder, load enabled libraries (hash re-checked), restore their inputs
            plugins.start();
            pluginsReady = true;
            media.syncPluginEffects(plugins);
            media.syncVst3Effects();  // scans the standard VST3 folders once at startup (no install/enable step)
            for (int i = 0; i < app::Engine::kSourceCount; ++i) {
                const auto e = assignments.get(i);
                std::string err;
                if (!e.empty() && e.enabled && e.id.rfind("plugin:", 0) == 0 && !attachPlugin(i, e.id, e.name, err))
                    engine.logger().warning("plugins", "entrada " + std::to_string(i + 1) + " não restaurada: " + err);
            }
            host.postQueued("plugins", plugins.listJson());
            host.postQueued("theme", plugins.themeJson());
            scan();
        });
        scan();
        feeder = std::thread([&] {
            win::trace("feeder thread start");
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            win::JpegEncoder jpeg;
            jpeg.init();
            video::Frame frame;
            video::Frame previewFull;  // full-resolution (not the small UI monitor size) for prevWin
            std::vector<std::uint8_t> bytes;
            video::Frame remoteThumb;                 // FEATURE: remote
            std::vector<std::uint8_t> remoteBytes;    // FEATURE: remote
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
                // Outputs: second/third-monitor windows and the NDI sender want the FULL-resolution
                // Program/Preview, not the small UI monitor size sendFrame() above produces.
                auto progLatest = (progWin.isOpen() || ndiOutputEnabled) ? engine.program().latest() : nullptr;
                if (progWin.isOpen() && progLatest) progWin.present(*progLatest);
                if (prevWin.isOpen()) {
                    const auto& pf = engine.program().format();
                    video::prepareFrame(previewFull, pf.width, pf.height);
                    engine.switcher().renderPreview(previewFull, tick);
                    overlays.renderPreview(previewFull);   // FEATURE: overlays (only when the option is on)
                    prevWin.present(previewFull);
                }
                if (ndiOutputEnabled && progLatest) {
                    std::lock_guard lock(ndiSenderMutex);
                    if (ndiSender) {
                        const auto& pf = engine.program().format();
                        ndiSender->feed(progLatest->data.data(), progLatest->width, progLatest->height,
                                       progLatest->stride, pf.fps.num, pf.fps.den);
                    }
                }
                // ---- FEATURE: remote ----
                // Remote devices: compact state ~7.5 Hz (only if someone is connected; the server also
                // drops it when unchanged) and opt-in low-res thumbnails ~2 fps. Everything here only
                // stores the latest value in the server's mailbox - it never touches a socket.
                if (tick % 2 == 0 && remoteSvc.wantsState()) {
                    hb::remote::RemoteState rs;
                    const auto sw = engine.switcher().state();
                    rs.pvw = sw.pvw; rs.pgm = sw.pgm; rs.transitioning = sw.transitioning; rs.progress = sw.progress;
                    rs.recording = media.recording();
                    for (int i = 0; i < app::Engine::kSourceCount; ++i) {
                        const auto info = engine.slotInfo(i);
                        const auto assigned = assignments.get(i);
                        const bool enabled = assigned.empty() || assigned.enabled;
                        hb::remote::RemoteSource src;
                        src.name = "FONTE " + std::to_string(i + 1);
                        src.status = "test";
                        if (info.live) {
                            using S = video::SourceStatus::State;
                            src.status = info.status.state == S::Online ? "online" : info.status.state == S::Connecting ? "connecting"
                                       : info.status.state == S::NoSignal ? "nosignal" : "error";
                            if (!info.label.empty()) src.name = info.label;
                        } else if (!assigned.empty() && !enabled) {
                            src.status = "off";
                            if (!assigned.name.empty()) src.name = assigned.name;
                        }
                        rs.sources.push_back(std::move(src));
                    }
                    const auto as = media.audio().snapshot();
                    for (const auto& st : as.strips)
                        rs.strips.push_back({st.name, st.faderDb, st.mute, st.solo, std::max(st.peakDb[0], st.peakDb[1])});
                    rs.masterPeakDb = std::max(as.master.levels.peakDb[0], as.master.levels.peakDb[1]);
                    // Destinations: name/platform/run state only - never the server URL or the key.
                    for (const auto& d : destinations.list()) {
                        hb::remote::RemoteDest rd{d.id, d.name, d.platform, false, false};
                        std::lock_guard lock(sendersMutex);
                        auto it = senders.find(d.id);
                        if (it != senders.end()) {
                            const auto st = it->second->status();
                            rd.running = st.running;
                            rd.publishing = st.publishing;
                        }
                        rs.dests.push_back(std::move(rd));
                    }
                    remoteSvc.pushState(rs);
                }
                if (tick % 8 == 0 && remoteSvc.wantsThumbs()) {
                    for (int kind = 0; kind < 2; ++kind) {
                        if (!engine.renderUiFrame(kind ? "pgm" : "pvw", frame)) continue;
                        video::downscaleBox(frame, remoteThumb, 240, 135);
                        if (jpeg.encode(remoteThumb, 55, remoteBytes)) remoteSvc.pushThumb(kind, remoteBytes);
                    }
                }
                if (tick % 8 == 0) host.postQueued("remote", remoteSvc.statusJson());
                // ---- /FEATURE ----
                // Input tiles: half of them per tick (~7.5 fps each) to save CPU.
                for (int i = static_cast<int>(tick % 2); i < app::Engine::kSourceCount; i += 2)
                    sendFrame(("s" + std::to_string(i)).c_str(), 70);
                // FEATURE: scenes - CENAS tiles ("cN"; empty slots render nothing and are skipped)
                for (int i = static_cast<int>(tick % 2); i < app::Engine::kSceneCount; i += 2)
                    sendFrame(("c" + std::to_string(i)).c_str(), 70);
                {
                    int tid; std::string ttxt;
                    { std::lock_guard lock(toastMutex); tid = toastId; ttxt = toastText; }
                    host.postQueued("state", stateJson(engine, vcam, assignments, tid, ttxt));
                    media.tick(tick, [&](const char* key, const std::string& json) { host.postQueued(key, json); });
                    const unsigned v = ptzSettings.version();
                    if (ptzSent.load() != v) { ptzSent = v; host.postQueued("ptz", ptzSettings.uiJson()); }
                    if (tick % 4 == 0) postXmitStatus();
                    if (tick % 4 == 2) { chromaSvc.tick(); postChroma(); }   // FEATURE: chroma
                    if (tick % 4 == 3) postCloud(false);   // FEATURE: cloudbackup
                    if (tick % 4 == 3) { const std::string cj = captions.pollJson(false); if (!cj.empty()) host.postQueued("cap", cj); }   // FEATURE: captions
                    if (tick % 4 == 1) { std::lock_guard lk(overlayMutex); postOverlayState(false); }   // FEATURE: overlays (live clocks)
                }
                // ---- FEATURE: teleprompter ---- periodic re-sync / transitions (cheap: a few msgs per second)
                if (auto j = tpc.poll()) tpBroadcast("tp", *j);
                // ---- /FEATURE ----
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
    chatSvc.stop();        // FEATURE: chat - joins the chat/OAuth threads before the window goes away
    remoteSvc.disable();   // FEATURE: remote - closes the server threads before the engine goes away
    {
        std::lock_guard lock(devMutex);
        for (auto& kv : devices) kv.second->stop();   // close capture graphs before the engine goes away
        for (auto& kv : ndiDevices) kv.second->stop();
    }
    chromaSvc.shutdown();   // FEATURE: chroma - stop AI workers/download before the engine goes away
    if (feeder.joinable()) feeder.join();
    win::trace("feeder joined");
    tpPresenter.close();   // FEATURE: teleprompter
    tpc.flush();           // FEATURE: teleprompter
    progWin.close();
    prevWin.close();
    { std::lock_guard lock(ndiSenderMutex); ndiSender.reset(); }
    {
        std::map<std::string, std::unique_ptr<win::StreamSender>> doomed;
        { std::lock_guard lock(sendersMutex); doomed = std::move(senders); }
        for (auto& kv : doomed) kv.second->stop();   // joins each sender's thread; while program_/audio_ still exist
    }
    captions.shutdown();   // FEATURE: captions - stops the worker + removes the audio tap while the engine still exists
    media.stop();     // finishes an ongoing recording (valid file) and saves the mixer settings
    cloudSvc.stop();  // FEATURE: cloudbackup - after media.stop() so the last file is journaled for the next launch
    win::trace("media stopped");
    vcam.shutdown();
    win::trace("vcam shut down");
    engine.stop();
    win::trace("engine stopped");
    CoUninitialize();
    return rc;
}
