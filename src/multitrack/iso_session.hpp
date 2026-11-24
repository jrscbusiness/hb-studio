#pragma once
// One multitrack recording session: owns the session folder layout, the ISO tracks, the camera samplers,
// the low-disk guard (ISO tracks stop FIRST, the Program recorder keeps its own, lower, threshold) and
// session.json / EDL writing. Pure C++20, writers injected through the WriterRegistry (or a test hook).
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/event_bus.hpp"
#include "core/logger.hpp"
#include "multitrack/frame_sampler.hpp"
#include "multitrack/iso_track.hpp"

namespace hb::multitrack {

struct IsoVideoSpec {
    int slot = 0;                   // camera slot (0-based)
    std::string name, device;
    int width = 1920, height = 1080, fpsNum = 30, fpsDen = 1;
    int bitrateKbps = 0;            // 0 = automatic
    VideoSampler::RenderFn render;  // renders the CLEAN camera into the frame
};

struct IsoAudioSpec {
    int strip = 0;                  // mixer strip (0-based)
    std::string name;
    std::string profileId = "wav";  // registry id of an audio-only format
    bool postFader = false;
    int sampleRate = 48000, channels = 2;
};

struct IsoSessionConfig {
    std::string sessionDir;         // absolute, created if missing
    std::string sessionName;
    std::string appVersion;
    std::vector<std::string> videoProfileIds{"mp4-h264-iso", "mp4-h264-aac"};   // first available wins
    std::vector<IsoVideoSpec> video;
    std::vector<IsoAudioSpec> audio;
    std::vector<std::string> warnings;          // notes to carry into session.json (e.g. cameras skipped)
    int rotationSeconds = 0;
    int videoQueueDepth = 6;       // frames queued per camera before the oldest is dropped (counted)
    std::uint64_t minFreeBytes = 2ull << 30;   // below this the ISO tracks stop (must exceed the Program's)
    int tcFpsNum = 30, tcFpsDen = 1;           // timeline rate of the EDL lists for audio
    bool writeEdl = true;
    core::EventBus* bus = nullptr;
    core::Logger* log = nullptr;

    // ---- test hooks ----
    SessionClock::NowFn nowNs;
    SessionClock::WallFn wallMs;
    std::function<std::uint64_t(const std::string&)> freeSpace;   // default: filesystem::space (unknown = huge)
    std::function<bool(const std::string&, rec::RecordingProfile&, rec::WriterFactory&)> findProfile;  // default: registry
    bool autoSupervise = true;      // false: the test calls superviseOnce()
    bool startSamplers = true;      // false: the test pushes frames itself
};

struct IsoSessionStatus {
    bool active = false;
    double seconds = 0;
    std::uint64_t freeBytes = 0;
    std::string sessionDir;
    std::vector<TrackStatus> tracks;
    std::vector<std::string> warnings;
};

class IsoSession {
public:
    explicit IsoSession(IsoSessionConfig cfg);
    ~IsoSession();
    IsoSession(const IsoSession&) = delete;
    IsoSession& operator=(const IsoSession&) = delete;

    // Creates iso/ and audio/, starts the clock, opens every track (a track that fails stays in Error; the
    // others go on). Fails only when nothing at all could be started.
    bool start(std::string& error);
    void stop();                                    // finalises everything, writes session.json + EDLs. Idempotent

    // Audio block of `frames` that JUST arrived (stamped now - duration). Never blocks.
    void pushAudioNow(std::size_t audioIndex, const float* interleaved, std::size_t frames);
    void pushAudio(std::size_t audioIndex, const float* interleaved, std::size_t frames, Ns startNs);
    void pushVideo(std::size_t videoIndex, std::shared_ptr<const video::Frame> f, Ns captureNs);

    void superviseOnce();                           // disk guard + periodic session.json
    // Information about the Program recording, merged into the manifest as track "program".
    void setProgramTrack(const ManifestTrack& t);

    IsoSessionStatus status() const;
    SessionManifest manifest(const std::string& state) const;
    const SessionClock& clock() const { return clock_; }
    std::string sessionDir() const { return cfg_.sessionDir; }
    bool writeManifest(const std::string& state);

private:
    void superviseLoop();

    IsoSessionConfig cfg_;
    SessionClock clock_;
    std::vector<std::unique_ptr<IsoVideoTrack>> vtracks_;
    std::vector<std::unique_ptr<IsoAudioTrack>> atracks_;
    std::vector<std::unique_ptr<VideoSampler>> samplers_;
    std::vector<std::string> warnings_;
    ManifestTrack program_;
    bool haveProgram_ = false;
    mutable std::mutex mu_;
    std::atomic<bool> started_{false}, stopped_{false};
    std::atomic<std::uint64_t> freeBytes_{0};
    std::int64_t lastManifestNs_ = 0;
    std::thread super_;
    std::mutex superMu_;
    std::condition_variable superCv_;
    bool superStop_ = false;
    Ns endNs_ = 0;
};

}  // namespace hb::multitrack
