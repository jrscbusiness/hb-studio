#pragma once
// Recorder: records the Program Output (video + audio) with any registered IMediaWriter. See docs/RECORDING.md.
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/event_bus.hpp"
#include "audio/resampler.hpp"
#include "core/logger.hpp"
#include "recording/media_writer.hpp"
#include "video/program_output.hpp"

namespace hb::rec {

struct RecorderConfig {
    std::string profileId;                          // id registered in WriterRegistry
    std::string folderUtf8;                         // output folder (created if missing)
    std::string nameTemplate = "HBStudio_{date}_{time}";  // {date} 2026-10-21, {time} 14-05-33, {n} counter
    // If profile.id is not empty its tunables (bitrates, quality, bits per sample, keyframe interval)
    // override those of the registered profile; id/label/extension/hasVideo/hasAudio always come from
    // the registry.
    RecordingProfile profile;
    VideoFormat video;
    // Format of the audio PUSHED to pushAudio() (normally 48 kHz stereo float). The Recorder converts it to
    // profile.audioSampleRate (44100/48000/96000) with hb::audio::Resampler (High) before the writer, which
    // receives AudioFormat{profile.audioSampleRate, channels} and never resamples.
    AudioFormat audio;
    std::int64_t splitSeconds = 0;                  // 0 = no split by time
    std::uint64_t splitBytes = 0;                   // 0 = no split by size (WAV/AVI are capped at 3.9 GB anyway)
    std::uint64_t minFreeBytes = 500ull << 20;      // below this the file is finalised and state = Error

    // ---- tunables / test hooks (defaults are right for the application) ----
    int videoQueueFrames = 8;                       // bounded video queue; oldest frame dropped when full
    double audioRingSeconds = 2.0;                  // bounded audio ring; oldest audio replaced by silence
    int audioStallToleranceMs = 1000;               // audio missing for longer than this is replaced by silence
    std::function<std::int64_t()> nowNs;            // monotonic clock; default std::chrono::steady_clock
    std::function<std::uint64_t(const std::string& folderUtf8)> freeSpaceBytes;  // default: filesystem::space
};

class Recorder {
public:
    explicit Recorder(RecorderConfig config, core::EventBus* bus = nullptr, core::Logger* log = nullptr);
    ~Recorder();
    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;

    // Opens the first file synchronously (so errors are reported here) and starts the writer thread.
    bool start(std::string& error);
    // Blocks until the writer thread has drained the queues and finalised the file(s). Idempotent.
    void stop();

    // Never block: they copy/queue in O(1) under a short lock and drop (counting) when the bounded
    // buffers are full. Safe to call from any thread, in any state (ignored unless Recording).
    void pushVideo(std::shared_ptr<const video::Frame> frame);
    void pushAudio(const float* interleaved, std::size_t frames);

    struct Status {
        enum class State { Idle, Recording, Stopping, Error };
        State state = State::Idle;
        std::string file;                // current (or last) file, full path
        double seconds = 0;              // media time written
        std::uint64_t videoFrames = 0;   // frame slots written (includes repeatedVideo)
        std::uint64_t repeatedVideo = 0; // slots filled by repeating the previous frame
        std::uint64_t droppedVideo = 0;  // frames discarded (queue full / wrong size)
        std::uint64_t audioFrames = 0;   // sample frames written (includes silence fill)
        std::uint64_t droppedAudio = 0;  // sample frames discarded (ring full); replaced by silence
        std::uint64_t bytes = 0;         // bytes written, all segments
        std::uint64_t freeBytes = 0;     // free space at the last disk check
        int segment = 0;                 // 1-based number of the current file
        std::string error;
        std::uint64_t silenceFrames = 0; // sample frames of silence inserted (overruns + stalls)
    };
    Status status() const;

private:
    void run();
    void loopOnce(bool final);
    bool step(bool final);
    bool emitSlot(std::uint64_t slot, std::uint64_t audioEnd, bool final);
    bool writeStageAudio(std::uint64_t upToFrame, bool final);
    bool rollSegment();
    bool openSegment(std::string& error);
    void closeWriter(std::string& error);
    void fail(const std::string& message);
    bool checkDisk(bool force);
    void publish(const std::string& topic, core::Fields fields);
    void log(core::Level level, const std::string& message);
    void updateStatus();
    std::int64_t now() const { return cfg_.nowNs(); }
    std::int64_t slotPtsNs(std::uint64_t slotInSegment) const;
    std::int64_t audioPtsNs(std::uint64_t frameInSegment) const;
    std::uint64_t slotEndAudioFrame(std::uint64_t slot) const;
    std::string segmentPath(int segment);

    RecorderConfig cfg_;
    core::EventBus* bus_;
    core::Logger* logger_;
    RecordingProfile profile_;
    WriterFactory factory_;
    bool hasVideo_ = false, hasAudio_ = false;
    int channels_ = 0, rate_ = 0 /* output rate (timeline) */, inRate_ = 0;
    std::unique_ptr<audio::Resampler> resampler_;
    std::vector<float> resampleOut_;
    std::uint64_t splitBytesEff_ = 0;

    // control
    std::mutex ctlMu_;
    std::thread thread_;
    std::atomic<int> state_{0};
    std::atomic<bool> stopReq_{false};

    // producer side (guarded by mu_)
    mutable std::mutex mu_;
    std::condition_variable cv_;
    bool dirty_ = false;
    std::deque<std::shared_ptr<const video::Frame>> vq_;
    bool firstVideo_ = false;
    std::int64_t firstVideoNs_ = 0;
    std::vector<float> ring_;
    std::size_t ringHead_ = 0, ringCount_ = 0, ringCap_ = 0;
    std::uint64_t pushedAbs_ = 0, nextAbs_ = 0;
    std::atomic<std::uint64_t> droppedVideo_{0}, droppedAudio_{0};

    // writer thread state (only touched by the writer thread after start())
    std::unique_ptr<IMediaWriter> writer_;
    struct StageChunk {
        std::vector<float> data;     // interleaved (empty for silence)
        std::uint64_t frames = 0, used = 0;
        bool silence = false;
    };
    std::deque<StageChunk> stage_;   // drained from the ring, not yet handed to the writer
    std::uint64_t stageFrames_ = 0;
    std::uint64_t audioWritten_ = 0; // timeline frames handed to writers (global)
    std::uint64_t slots_ = 0;        // video slots written (global)
    std::uint64_t repeated_ = 0, silence_ = 0;
    std::uint64_t segAudioBase_ = 0, segSlotBase_ = 0;
    std::uint64_t bytesDone_ = 0;
    std::shared_ptr<const video::Frame> lastFrame_;
    int segment_ = 0;
    std::string baseName_, folder_;
    std::int64_t lastDiskCheckNs_ = 0;
    std::int64_t t0Ns_ = 0;
    bool haveT0_ = false;
    std::thread::id writerThreadId_;
    std::vector<float> zeros_;

    mutable std::mutex statusMu_;
    Status status_;
};

}  // namespace hb::rec
