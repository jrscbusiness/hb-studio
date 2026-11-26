#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "app/stream_destinations.hpp"
#include "audio_io/audio_engine.hpp"
#include "streaming/stream_stats.hpp"
#include "streaming/windows/mf_encoder.hpp"
#include "video/program_output.hpp"

// Drives one outbound RTMP(S) connection (with automatic reconnect/backoff and optional adaptive bitrate) for one StreamDestination: taps the Program video/audio
// (same addConsumer()/addProgramConsumer() pattern rec::Recorder already uses), encodes it, and
// publishes it. Runs entirely on its own thread, started by start() and torn down by stop()/the
// destructor, so a stuck/slow/failed destination can never affect the others or the real-time
// audio/video threads - those only ever push a cheap copy into a small bounded queue here, never
// block on the network or on encoding.
namespace hb::win {

class StreamSender {
public:
    StreamSender(hb::app::StreamDestination dest, video::ProgramOutput& program, audio_io::AudioEngine& audio);
    ~StreamSender();
    StreamSender(const StreamSender&) = delete;
    StreamSender& operator=(const StreamSender&) = delete;

    void start();   // idempotent; a second call while already running does nothing
    void stop();    // blocks until the sender thread has fully unwound

    struct Status {
        bool running = false;      // start() was called and hasn't finished/failed yet
        bool publishing = false;   // actually connected to the server and sending media
        std::string message;       // pt-BR, last status/error - always set, even on success
        std::uint64_t videoUnits = 0, audioUnits = 0;
        bool reconnecting = false;   // connection lost / not yet up, waiting for the next attempt
        bool adaptive = false;       // adaptive bitrate enabled for this destination
        hb::streaming::StreamStats::Snapshot stats;   // live health (bitrate, drops, queues...)
        std::chrono::steady_clock::time_point startedAt{};
    };
    Status status() const;

private:
    void run();
    void setStatus(bool running, bool publishing, std::string message);

    hb::app::StreamDestination dest_;
    video::ProgramOutput& program_;
    audio_io::AudioEngine& audio_;

    std::thread thread_;
    std::atomic<bool> stopFlag_{false};

    mutable std::mutex statusMutex_;
    Status status_;
    mutable hb::streaming::StreamStats stats_;
    std::atomic<std::uint64_t> droppedVideoTotal_{0};

    video::ProgramOutput::ConsumerId vConsumer_ = 0;
    audio_io::AudioEngine::ConsumerId aConsumer_ = 0;

    // Bounded hand-off queues: the consumer callbacks (clock/audio engine threads) push and must
    // never block; this sender's own thread pops. Oldest entries are dropped on overflow - a
    // destination that can't keep up falls behind in time rather than growing memory without bound
    // or stalling the rest of the app.
    static constexpr std::size_t kMaxQueuedVideo = 90;   // ~3s at 30fps
    static constexpr std::size_t kMaxQueuedAudio = 300;  // ~3s of 10ms blocks
    std::mutex queueMutex_;
    std::condition_variable queueCv_;
    std::deque<std::shared_ptr<const video::Frame>> videoQueue_;
    std::deque<std::vector<float>> audioQueue_;   // each entry: one engine block, interleaved
    int audioChannels_ = 2;
};

}  // namespace hb::win
