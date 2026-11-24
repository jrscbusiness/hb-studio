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

#include "app/engine.hpp"
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
#include "video/frame_ops.hpp"
#include "plugins/service.hpp"
#include "vst3/vst3.hpp"

using namespace hb;

namespace {

std::string stateJson(app::Engine& engine, win::VirtualCameraController& vcam, const app::SourceAssignments& assignments,
                      int toastId, const std::string& toast) {
    const auto s = engine.snapshot();
    const auto v = vcam.status();
    const auto pf = engine.program().format();
    char b[2048];
    snprintf(b, sizeof b,
             "{\"t\":\"s\",\"pvw\":%d,\"pgm\":%d,\"transition\":%s,\"progress\":%.3f,\"fps\":%.2f,\"target\":%d,"
             "\"pw\":%d,\"ph\":%d,"
             "\"ver\":\"" HB_VERSION "\",\"frames\":%llu,\"skipped\":%llu,\"tier\":\"%s\",\"cores\":%u,\"ramGiB\":%u,\"srcRec\":%d,\"srcMax\":%d,"
             "\"vcam\":{\"supported\":%s,\"installed\":%s,\"active\":%s,\"connected\":%s,\"busy\":%s,\"error\":%s,"
             "\"sent\":%llu,\"message\":\"%s\"}",
             s.sw.pvw, s.sw.pgm, s.sw.transitioning ? "true" : "false", s.sw.progress, s.fps, s.targetFps,
             pf.width, pf.height,
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
        const auto assigned = assignments.get(i);
        // A disabled slot has been detach()'d (engine.slotInfo reports not live, same as an empty
        // slot), so the remembered device (id/name from SourceAssignments) must be overlaid here -
        // otherwise turning a camera off would look identical to never having assigned one.
        const bool enabled = assigned.empty() || assigned.enabled;
        const char* k = "test";
        const char* st = "test";
        std::string label, devId, msg;
        if (info.live) {
            using S = video::SourceStatus::State;
            k = "dev";
            st = info.status.state == S::Online ? "online" : info.status.state == S::Connecting ? "connecting"
               : info.status.state == S::NoSignal ? "nosignal" : "error";
            label = info.label;
            devId = info.deviceId;
            msg = info.status.message;
        } else if (!assigned.empty() && !enabled) {
            k = "dev";
            st = "off";
            label = assigned.name;
            devId = assigned.id;
        }
        char sb[160];
        snprintf(sb, sizeof sb, "%s{\"k\":\"%s\",\"st\":\"%s\",\"en\":%s,\"w\":%d,\"h\":%d,\"fps\":%.1f,\"name\":\"", i ? "," : "",
                 k, st, enabled ? "true" : "false", info.status.width, info.status.height, info.status.fps);
        j += sb + win::jsonEscape(label) + "\",\"id\":\"" + win::jsonEscape(devId) + "\",\"msg\":\"" + win::jsonEscape(msg) + "\"}";
    }
    j += "]";
    j += ",\"log\":[";
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

    // ---- Program output format: a saved choice from CONFIG > Formatos de saída comuns, read
    // before the engine exists since ProgramOutput's resolution/fps is fixed for its whole
    // lifetime (changing it live would mean resizing every consumer - switcher sources, the
    // virtual camera, recording, NDI/RTMP output - while they run; out of scope for now). A
    // changed format only takes effect the next time HB Studio starts, which "outfmt"'s toast says.
    video::ProgramFormat outputFormat{1920, 1080, {30, 1}};
    {
        PWSTR p = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p))) {
            std::ifstream f(win::narrow(std::wstring(p) + L"\\HBStudio\\outputformat.txt"), std::ios::binary);
            CoTaskMemFree(p);
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
    // ---- TRANSMISSÃO: outbound RTMP(S) destinations, persisted next to the other settings ----
    app::StreamDestinations destinations;
    std::string destPath;
    {
        PWSTR p = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p))) {
            destPath = win::narrow(std::wstring(p) + L"\\HBStudio\\xmit.json");
            CoTaskMemFree(p);
        }
        if (!destPath.empty()) destinations.load(destPath);
    }
    std::mutex sendersMutex;
    std::map<std::string, std::unique_ptr<win::StreamSender>> senders;   // destination id -> running sender
    // ---- output format (CONFIG > Formatos de saída comuns): same file read at the top into
    // `outputFormat`, kept here too so the "outfmt" command can save a new choice for next launch.
    std::string outFmtPath;
    {
        PWSTR p = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p))) {
            outFmtPath = win::narrow(std::wstring(p) + L"\\HBStudio\\outputformat.txt");
            CoTaskMemFree(p);
        }
    }
    // ---- plugins (themes + native video-source plugins), installed per user ----
    std::string pluginsDir;
    {
        PWSTR p = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p))) {
            pluginsDir = win::narrow(std::wstring(p) + L"\\HBStudio\\plugins");
            CoTaskMemFree(p);
        }
    }
    plugins::PluginService plugins(pluginsDir, "windows-x64");
    std::atomic<bool> pluginsReady{false};
    // ---- audio mixer + recording ----
    std::string mediaDir, videosDir;
    {
        PWSTR p = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p))) {
            mediaDir = win::narrow(std::wstring(p) + L"\\HBStudio");
            CoTaskMemFree(p);
        }
        PWSTR v = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Videos, 0, nullptr, &v))) {
            videosDir = win::narrow(std::wstring(v) + L"\\HB Studio");
            CoTaskMemFree(v);
        }
    }
    app::MediaService media(engine.program(), &engine.bus(), &engine.logger(), mediaDir, videosDir);
    media.start();
    win::trace("media service started");

    // ---- native dark/light mode (built into the app; no plugin needed) ----
    const std::string themePath = mediaDir.empty() ? std::string() : mediaDir + "\\theme.txt";
    auto effectiveDark = [](const std::string& pref) {
        if (pref == "dark" || pref == "gray") return true;  // "gray" is a dark-ish neutral palette too
        if (pref == "light") return false;
        return win::systemPrefersDark();  // "system" (default)
    };
    std::string themePref = "system";
    if (!themePath.empty()) {
        std::ifstream f(themePath, std::ios::binary);
        std::string v;
        if (f && std::getline(f, v) && (v == "dark" || v == "light" || v == "gray" || v == "system")) themePref = v;
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
    // ---- FEATURE: ai ----
    // AI providers (CONFIG > Inteligência Artificial). Declared after `host` so worker threads are joined
    // (AiService destructor) before the window goes away. Keys: DPAPI-encrypted file, never sent to the UI.
    std::unique_ptr<hb::ai::ISecretStore> aiSecrets = hb::ai::makeDefaultSecretStore();
    hb::ai::ProviderStore aiStore(aiSecrets.get());
    std::string aiPath;
    {
        PWSTR p = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p))) {
            aiPath = win::narrow(std::wstring(p) + L"\\HBStudio\\ai.json");
            CoTaskMemFree(p);
        }
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
        const std::size_t maxParts = c0 == "ptzname" ? 4 : (c0 == "ptzcfg" ? 3 : (c0 == "ptzall" ? 2 :
                                     (c0 == "devassign" ? 3 : (c0 == "devformats" ? 2 : (c0 == "xmitsave" ? 2 : 8)))));
        auto a = split(msg, maxParts);
        if (a.empty()) return;
        auto idx = [&](std::size_t k) { return k < a.size() ? std::atoi(a[k].c_str()) : -1; };
        const std::string& c = a[0];
        const int cam = idx(1);
        const std::string camLabel = "CAM " + std::to_string(cam + 1) + ": ";
        // ---- FEATURE: ai ----
        if (c0.rfind("ai", 0) == 0 && aiCommands.handle(msg)) return;
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
            host.postQueued("previewq", "{\"t\":\"previewq\",\"id\":\"" + win::jsonEscape(engine.previewQuality()) + "\"}");
        }
        else if (c == "uitheme" && a.size() >= 2 && (a[1] == "light" || a[1] == "dark" || a[1] == "gray" || a[1] == "system")) {
            // Native dark/light mode: built into the app, no plugin needed. The web page applies
            // its own colours (CSS + localStorage, see ui/index.html); this just keeps the native
            // window chrome (title bar) in sync and remembers the choice for the next launch.
            themePref = a[1];
            host.setTheme(effectiveDark(themePref));
            if (!themePath.empty()) {
                std::ofstream f(themePath, std::ios::binary | std::ios::trunc);
                f << themePref;
            }
        }
        else if (c == "outwin" && a.size() >= 3) {
            // Second/third monitor output: a plain window the operator drags to another screen and
            // maximizes/fullscreens (F11) there — see src/app/windows/output_window.hpp.
            win::OutputWindow& win_ = a[1] == "preview" ? prevWin : progWin;
            if (a[2] == "open") win_.open(inst);
            else if (a[2] == "close") win_.close();
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
            } else {
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
        else if (c == "preview") engine.switcher().setPreview(idx(1));
        else if (c == "program") engine.switcher().setProgram(idx(1));
        else if (c == "cut") engine.switcher().cut();
        else if (c == "dissolve") engine.switcher().startDissolve(std::clamp(idx(1) > 0 ? idx(1) : 500, 50, 10000));
        else if (c == "tbar" && a.size() >= 2) engine.switcher().setTBarPosition(static_cast<float>(std::atof(a[1].c_str())));
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
        } else if (c == "xmitsave" && a.size() >= 2) {
            app::StreamDestination d;
            if (app::StreamDestinations::parseDestination(a[1], d)) {
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
        } else if (c == "xmitremove" && a.size() >= 2) {
            stopXmit(a[1]);
            if (destinations.remove(a[1])) {
                if (!destPath.empty()) destinations.save(destPath);
                postXmitDest();
                postXmitStatus();
            }
        } else if (c == "xmitenable" && a.size() >= 3) {
            if (destinations.setEnabled(a[1], a[2] == "1")) {
                if (!destPath.empty()) destinations.save(destPath);
                if (a[2] != "1") stopXmit(a[1]);   // turning a destination off also stops it if running
                postXmitDest();
                postXmitStatus();
            }
        } else if (c == "xmitstart" && a.size() >= 2) {
            startXmit(a[1]);
            postXmitStatus();
        } else if (c == "xmitstop" && a.size() >= 2) {
            stopXmit(a[1]);
            postXmitStatus();
        } else if (c == "xmitstartall") {
            for (const auto& d : destinations.list())
                if (d.enabled) startXmit(d.id);
            postXmitStatus();
        } else if (c == "xmitstopall") {
            for (const auto& d : destinations.list()) stopXmit(d.id);
            postXmitStatus();
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
        } else if (c == "previewq" && a.size() >= 2) {
            if (engine.setPreviewQuality(a[1])) {
                if (!previewQPath.empty()) {
                    std::ofstream f(previewQPath, std::ios::binary | std::ios::trunc);
                    f << a[1];
                }
                host.postQueued("previewq", "{\"t\":\"previewq\",\"id\":\"" + win::jsonEscape(a[1]) + "\"}");
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
        } else if (c == "plugincancel" && a.size() >= 2) {
            plugins.cancelInstall(a[1]);
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
                // Input tiles: half of them per tick (~7.5 fps each) to save CPU.
                for (int i = static_cast<int>(tick % 2); i < app::Engine::kSourceCount; i += 2)
                    sendFrame(("s" + std::to_string(i)).c_str(), 70);
                {
                    int tid; std::string ttxt;
                    { std::lock_guard lock(toastMutex); tid = toastId; ttxt = toastText; }
                    host.postQueued("state", stateJson(engine, vcam, assignments, tid, ttxt));
                    media.tick(tick, [&](const char* key, const std::string& json) { host.postQueued(key, json); });
                    const unsigned v = ptzSettings.version();
                    if (ptzSent.load() != v) { ptzSent = v; host.postQueued("ptz", ptzSettings.uiJson()); }
                    if (tick % 4 == 0) postXmitStatus();
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
        for (auto& kv : ndiDevices) kv.second->stop();
    }
    if (feeder.joinable()) feeder.join();
    win::trace("feeder joined");
    progWin.close();
    prevWin.close();
    { std::lock_guard lock(ndiSenderMutex); ndiSender.reset(); }
    {
        std::map<std::string, std::unique_ptr<win::StreamSender>> doomed;
        { std::lock_guard lock(sendersMutex); doomed = std::move(senders); }
        for (auto& kv : doomed) kv.second->stop();   // joins each sender's thread; while program_/audio_ still exist
    }
    media.stop();     // finishes an ongoing recording (valid file) and saves the mixer settings
    win::trace("media stopped");
    vcam.shutdown();
    win::trace("vcam shut down");
    engine.stop();
    win::trace("engine stopped");
    CoUninitialize();
    return rc;
}
