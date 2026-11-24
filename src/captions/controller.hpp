#pragma once
// UI-facing controller of "Legendas ao vivo": handles the "cap|..." host commands, owns the
// CaptionService lifecycle, the model download, the settings file, the on-screen transcript and the
// .srt export. Portable (the platform pieces - audio tap registration, HTTP, whisper factory - are
// injected), so it is unit-tested on Linux with fakes.
//
// Commands (all via handleCommand): cap|on  cap|off  cap|set|<key>|<value>  cap|dl|<modelId>
//                                   cap|dlcancel  cap|clear
// Threading: handleCommand from the UI thread, pollJson from the feeder thread, caption.text and
// recording.fileClosed handlers on worker threads. stateMu_ is only ever held briefly (never across a
// join or file I/O); ctlMu_ serialises start/stop.
#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "captions/cloud_recognizer.hpp"
#include "captions/model_manager.hpp"
#include "captions/service.hpp"
#include "captions/settings.hpp"
#include "core/event_bus.hpp"

namespace hb::captions {

struct ControllerDeps {
    hb::core::EventBus* bus = nullptr;
    std::string settingsPath;   // captions.json
    std::string modelsDir;      // %LOCALAPPDATA%\HBStudio\models
    std::function<std::string()> srtFolder;   // where a session .srt goes when no recording file is known
    IDownloader* downloader = nullptr;
    IHttpPoster* http = nullptr;
    // Program audio tap (AudioEngine::addProgramConsumer / removeProgramConsumer in the app).
    std::function<std::uint64_t(std::function<void(const float*, std::size_t)>)> addProgramConsumer;
    std::function<void(std::uint64_t)> removeProgramConsumer;
    // null when the build has no whisper.cpp (HB_CAPTIONS_WHISPER=OFF).
    std::function<std::unique_ptr<ISpeechRecognizer>(const std::string& modelPathUtf8)> makeWhisper;
    std::function<std::string()> timestamp;   // "YYYYMMDD_HHMMSS" (injectable for tests); default: local time
    std::vector<ModelInfo> catalog;           // empty = built-in
};

class CaptionController {
public:
    explicit CaptionController(ControllerDeps deps);
    ~CaptionController();
    CaptionController(const CaptionController&) = delete;
    CaptionController& operator=(const CaptionController&) = delete;

    // Returns true if `msg` was a "cap|..." command (handled or rejected).
    bool handleCommand(const std::string& msg);
    // JSON for the UI (key "cap"); "" if unchanged since the last call (unless force).
    std::string pollJson(bool force = false);
    std::string stateJson();
    void shutdown();   // stop captions + cancel download (idempotent)

    bool running() const { return running_.load(); }

private:
    bool startLocked();
    void stopLocked();
    void onCaption(const hb::core::Event& e);
    void onFileClosed(const hb::core::Event& e);
    void startDownload(const std::string& id);
    void writeSrt(const std::string& path, const std::string& text);
    std::string fallbackSrtPath();

    ControllerDeps d_;
    ModelManager models_;
    std::mutex ctlMu_;
    mutable std::mutex stateMu_;
    CaptionSettings settings_;
    std::shared_ptr<CaptionService> service_;
    std::uint64_t consumerId_ = 0;
    std::atomic<bool> running_{false};
    std::string message_;               // operator-facing note (e.g. "baixe o modelo")
    std::deque<std::string> lines_;     // last committed captions
    std::string partial_;
    std::string srtFile_;               // last .srt written
    std::string sessionSrtPath_;        // .srt next to the last closed program recording of this session
    hb::core::SubscriptionId capSub_ = 0, fileSub_ = 0;

    std::thread dlThread_;
    std::atomic<bool> dlCancel_{false};
    bool dlActive_ = false;
    std::string dlId_, dlError_;
    std::uint64_t dlGot_ = 0, dlTotal_ = 0;
    std::string lastPosted_;
};

}  // namespace hb::captions
