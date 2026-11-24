#pragma once
// IsoService: application layer of the multitrack ("Gravação multipista") feature. Portable (no Windows
// headers). Holds the user's ISO selection/settings (persisted in multitrack.json), turns "rec|iso|..."
// commands into setting changes, estimates the cost of the plan, and runs one IsoSession next to the
// Program recording: cameras are sampled CLEAN from their slots, audio channels are tapped from the mixer
// strips. MediaService calls begin()/end() around the Program recorder. See docs/GRAVACAO_MULTIPISTA.md.
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "audio_io/audio_engine.hpp"
#include "core/event_bus.hpp"
#include "core/hardware_profile.hpp"
#include "core/logger.hpp"
#include "multitrack/iso_session.hpp"
#include "recording/recorder.hpp"
#include "video/program_output.hpp"
#include "video/source.hpp"

namespace hb::app {

struct IsoCamera {
    int slot = 0;
    bool live = false;           // a real device is assigned (a test pattern is never recorded)
    std::string label;           // "CAM 3"
    std::string device;          // device name ("" when none)
    int width = 0, height = 0;   // native resolution reported by the device (0 = unknown)
    double fps = 0;              // measured camera fps (0 = unknown)
};

struct IsoSettings {
    std::set<int> cams;          // selected camera slots (0-based)
    std::set<int> chans;         // selected mixer strips (0-based)
    std::string res = "native";  // native | 1080 | 720 | 540
    std::string fps = "program"; // program | 24 | 25 | 30 | 50 | 60
    int videoKbps = 0;           // 0 = automatic
    std::string audioFormat = "wav";   // registry id of an audio-only format
    bool postFader = false;
    int rotateMin = 0;           // 0 = off
    bool expert = false;         // allow plans above the safe encode budget
};

class IsoService {
public:
    struct Providers {
        std::function<std::vector<IsoCamera>()> cameras;
        std::function<std::shared_ptr<const video::Source>(int slot)> source;   // the CLEAN camera
    };

    IsoService(audio_io::AudioEngine& audio, video::ProgramOutput& program, core::EventBus* bus, core::Logger* log,
               std::string dataDirUtf8, std::string appVersion = {});
    ~IsoService();

    void setProviders(Providers p);
    void load();
    void save();
    // Overrides the detected hardware (tests).
    void setHardware(core::Tier tier, bool hwEncode);

    // "rec|iso|<sub>|..." split by '|' (a[0]="rec", a[1]="iso"). Returns a toast ("" none). Never throws.
    std::string handle(const std::vector<std::string>& a, bool programRecording);

    bool enabled() const;              // at least one ISO track selected
    unsigned version() const { return ver_.load(); }

    // Refuses (false + pt-BR error) only when the estimate is Blocked. On success creates the session folder
    // `baseFolder/<date-time>`, starts the ISO tracks and returns the folder, the program file name template
    // and the segment length the Program recorder should use (0 = leave as configured).
    bool begin(const std::string& baseFolderUtf8, std::uint64_t programMinFreeBytes, double programKbps,
               int programFpsNum, int programFpsDen, std::string& sessionDirUtf8, int& programSplitSeconds,
               std::string& error);
    // Stops taps and tracks; merges the Program recording info into session.json.
    void end(const rec::Recorder::Status* programStatus, const std::string& programProfileId);
    bool active() const;
    std::string lastSessionDir() const;

    std::string stateJson(bool programRecording, const std::string& baseFolderUtf8, double programKbps,
                          std::uint64_t programMinFreeBytes);

private:
    struct Plan {
        std::vector<multitrack::IsoVideoSpec> video;
        std::vector<multitrack::IsoAudioSpec> audio;
        std::vector<std::string> notes;     // cameras skipped, etc.
    };
    Plan buildPlan(const IsoSettings& s, int programFpsNum, int programFpsDen) const;
    std::vector<IsoCamera> cameras() const;

    audio_io::AudioEngine& audio_;
    video::ProgramOutput& program_;
    core::EventBus* bus_;
    core::Logger* log_;
    std::string dataDir_, appVersion_;
    Providers prov_;
    core::Tier tier_;
    bool hwEncode_ = false;

    mutable std::mutex mu_;
    IsoSettings s_;
    std::atomic<unsigned> ver_{1};
    std::shared_ptr<multitrack::IsoSession> session_;
    std::vector<audio_io::AudioEngine::ConsumerId> taps_;
    std::string lastDir_;
    int programSplitSeconds_ = 0;
    core::SubscriptionId busSub_ = 0;
    std::vector<std::string> programFiles_;   // closed Program files of the current session (from the event bus)
    std::mutex progMu_;
};

}  // namespace hb::app
