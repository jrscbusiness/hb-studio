#pragma once
// One ISO track = one file series (video from a camera, or audio from a mixer channel) written by its own
// worker thread. Isolation rules: push*() never blocks (bounded queue, drop-oldest, counted honestly); a
// failing writer puts ONLY this track in Error; every finalised file publishes `recording.fileClosed`.
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/event_bus.hpp"
#include "core/logger.hpp"
#include "multitrack/session_clock.hpp"
#include "multitrack/session_manifest.hpp"
#include "multitrack/track_timeline.hpp"
#include "recording/media_writer.hpp"
#include "video/program_output.hpp"

namespace hb::multitrack {

enum class TrackKind { Video, Audio };
enum class TrackState { Idle, Recording, Finished, Error, DiskStopped };
const char* trackStateName(TrackState s);

struct TrackSpec {
    TrackKind kind = TrackKind::Video;
    std::string id;              // "CAM01" / "CH01"
    std::string name;            // user label
    std::string device;          // device name (cameras)
    int sourceIndex = -1;
    std::string folder;          // absolute directory of the files (must exist)
    std::string relDir;          // same directory relative to the session folder ("iso", "audio")
    std::string stem;            // file name without extension
    std::string codec;           // informational ("h264", "pcm_s24le", ...)
    std::string tap;             // audio: "pre-fader" | "post-fader"
    rec::RecordingProfile profile;
    rec::WriterFactory factory;
    rec::VideoFormat video;      // video tracks
    rec::AudioFormat audio;      // audio tracks
    int rotationSeconds = 0;     // 0 = off
    int queueDepth = 6;          // video: frames; audio: see maxQueuedSeconds
    double maxQueuedSeconds = 4.0;
    int wavSyncSeconds = 2;      // WAV: refresh the header this often so a crash leaves a readable file
};

struct TrackServices {
    const SessionClock* clock = nullptr;
    core::EventBus* bus = nullptr;
    core::Logger* log = nullptr;
};

struct TrackStatus {
    std::string id, name;
    TrackKind kind = TrackKind::Video;
    TrackState state = TrackState::Idle;
    std::string error;
    int segment = 0;
    std::uint64_t units = 0, repeated = 0, droppedQueue = 0, droppedTimeline = 0, silence = 0, jumped = 0;
    std::uint64_t bytes = 0;
    double seconds = 0;
    double driftPpm = 0;
    std::string currentFile;
};

class IsoTrack {
public:
    IsoTrack(TrackSpec spec, TrackServices services);
    virtual ~IsoTrack();
    IsoTrack(const IsoTrack&) = delete;
    IsoTrack& operator=(const IsoTrack&) = delete;

    // Opens the first file synchronously (errors come back here) and starts the worker.
    bool start(std::string& error);
    // Drains the queue, finalises the file, joins the worker. Idempotent.
    void stop();
    // Like stop() but the state becomes DiskStopped (low disk space).
    void stopForDisk(const std::string& reason);

    TrackStatus status() const;
    ManifestTrack manifest() const;
    const TrackSpec& spec() const { return spec_; }
    bool active() const { return state_.load() == static_cast<int>(TrackState::Recording); }

protected:
    virtual void workerLoop() = 0;
    virtual void fillStatus(TrackStatus& s) const = 0;
    virtual void fillManifest(ManifestTrack& t) const = 0;

    bool openSegment(std::uint64_t startUnit, Ns startNs, std::string& error);
    void closeSegment();                 // finalises + publishes recording.fileClosed
    void failTrack(const std::string& message);
    void log(core::Level l, const std::string& m);
    std::string segmentPath(int index) const;
    std::string relPath(int index) const;
    bool syncWavHeader(std::string& error);

    TrackSpec spec_;
    TrackServices svc_;
    std::unique_ptr<rec::IMediaWriter> writer_;   // worker thread only (after start)
    int segment_ = 0;
    bool userRotation_ = false;

    mutable std::mutex mu_;                       // guards queues (subclasses), segs_, error_, counters snapshot
    std::condition_variable cv_;
    std::vector<ManifestSegment> segs_;
    std::string error_;
    std::atomic<int> state_{static_cast<int>(TrackState::Idle)};
    std::atomic<bool> stopReq_{false};
    std::atomic<bool> diskStop_{false};
    std::thread thread_;
};

class IsoVideoTrack final : public IsoTrack {
public:
    IsoVideoTrack(TrackSpec spec, TrackServices services);
    ~IsoVideoTrack() override { stop(); }
    // `captureNs`: session time of the capture. Never blocks.
    void pushVideo(std::shared_ptr<const video::Frame> frame, Ns captureNs);

private:
    void workerLoop() override;
    void fillStatus(TrackStatus& s) const override;
    void fillManifest(ManifestTrack& t) const override;
    bool writeSlot(const video::Frame& f, std::uint64_t slot);

    struct Item {
        std::shared_ptr<const video::Frame> frame;
        Ns ns = 0;
    };
    std::deque<Item> q_;
    VideoTimeline tl_;
    std::uint64_t segBaseSlot_ = 0, segEndSlot_ = 0;
    bool segBaseSet_ = false;
    std::uint64_t segUnits_ = 0;
    std::atomic<std::uint64_t> droppedQueue_{0}, droppedSize_{0};
    std::atomic<std::uint64_t> units_{0}, repeated_{0}, droppedTl_{0}, jumped_{0};
    std::int64_t firstOffsetNs_ = 0;
    std::shared_ptr<const video::Frame> prev_;
};

class IsoAudioTrack final : public IsoTrack {
public:
    IsoAudioTrack(TrackSpec spec, TrackServices services);
    ~IsoAudioTrack() override { stop(); }
    // `startNs`: session time of the first sample of the block. Interleaved float, spec.audio.channels wide.
    void pushAudio(const float* interleaved, std::size_t frames, Ns startNs);

private:
    void workerLoop() override;
    void fillStatus(TrackStatus& s) const override;
    void fillManifest(ManifestTrack& t) const override;
    bool writeFrames(const float* data, std::uint64_t n, bool repeatLast);  // data == nullptr: silence

    struct Block {
        std::vector<float> data;
        std::uint64_t frames = 0;
        Ns startNs = 0;
        std::uint64_t lostBefore = 0;
    };
    std::deque<Block> q_;
    std::uint64_t queuedFrames_ = 0, pendingLost_ = 0;
    AudioTimeline tl_;
    std::uint64_t segBase_ = 0, segEnd_ = 0, segUnits_ = 0, pos_ = 0, sinceSync_ = 0;
    std::uint64_t rotFrames_ = 0;
    std::vector<float> zeros_, last_;
    std::atomic<std::uint64_t> droppedQueue_{0}, units_{0};
    std::atomic<std::uint64_t> silence_{0}, skipped_{0}, repeated_{0}, leading_{0};
    std::atomic<double> drift_{0};
    std::atomic<std::int64_t> elapsedNs_{0};
};

}  // namespace hb::multitrack
