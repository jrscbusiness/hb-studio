#pragma once
// MediaService: the application layer for AUDIO (mixer strips, effects) and RECORDING.
// Portable (no Windows headers): owns the AudioEngine and the Recorder, turns UI commands
// ("aud|...", "rec|...") into engine calls, persists settings, and produces the JSON the UI shows.
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>

#include "audio_io/audio_engine.hpp"
#include "plugins/service.hpp"
#include "recording/recorder.hpp"
#include "video/program_output.hpp"

namespace hb::app {

struct RecordingSettings {
    std::string profileId = "mp4-h264-aac";
    std::string folder;                       // UTF-8; default <Videos>\HB Studio
    std::string nameTemplate = "HBStudio_{date}_{time}";
    int videoBitrateKbps = 8000, videoQuality = 85, audioBitrateKbps = 192, audioSampleRate = 48000,
        audioBitsPerSample = 24, keyframeIntervalSec = 2;
    int splitMinutes = 0;                     // 0 = off
    int splitMegabytes = 0;                   // 0 = off
    int minFreeMegabytes = 500;
};

class MediaService {
public:
    using Post = std::function<void(const char* key, const std::string& json)>;

    // dataDir: where audio.json / recording.json live. defaultFolder: default recording folder.
    MediaService(video::ProgramOutput& program, core::EventBus* bus, core::Logger* log, std::string dataDirUtf8,
                 std::string defaultFolderUtf8, audio_io::EngineOptions audioOptions = {});
    ~MediaService();

    void start();   // loads settings, starts the audio engine
    void stop();    // stops recording (finalising the file), saves settings, stops the engine

    // Handles "aud|..." and "rec|..." commands. Returns a toast text ("" = none). Never throws.
    std::string handle(const std::string& message);

    // Called ~15x/s from the UI feeder: posts "audiolv" always, "audio" when the configuration changed,
    // "rec" ~4x/s, "recprofiles" when settings changed.
    void tick(std::uint64_t tickNo, const Post& post);
    void forceResend() { audioVersionSent_ = ~0u; recSettingsSent_ = ~0u; }

    // Effect plugins: (re)registers the effects of all enabled+loaded plugins in the EffectRegistry.
    void syncPluginEffects(plugins::PluginService& plugins);

    audio_io::AudioEngine& audio() { return audio_; }
    bool recording() const;
    RecordingSettings settings() const;
    bool setFolder(const std::string& folderUtf8);   // from the native folder dialog

private:
    bool startRecording(std::string& error);
    void stopRecording();
    std::string recStatusJson();
    std::string recProfilesJson();
    void loadSettings();
    void saveSettings();
    void saveSettingsLocked();

    video::ProgramOutput& program_;
    core::EventBus* bus_;
    core::Logger* log_;
    std::string dataDir_, defaultFolder_;
    audio_io::AudioEngine audio_;

    mutable std::mutex mu_;
    RecordingSettings rs_;
    unsigned recSettingsVer_ = 1;
    std::shared_ptr<rec::Recorder> recorder_;
    video::ProgramOutput::ConsumerId vConsumer_ = 0;
    audio_io::AudioEngine::ConsumerId aConsumer_ = 0;
    bool haveV_ = false, haveA_ = false;
    std::set<std::string> pluginFxSources_;
    std::string lastRecError_, lastFile_;

    unsigned audioVersionSent_ = ~0u, recSettingsSent_ = ~0u;
    std::uint64_t lastSaveCheck_ = 0;
    unsigned audioVersionSaved_ = 0;
    bool started_ = false;
};

}  // namespace hb::app
