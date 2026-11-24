#pragma once
// Threaded wrapper around CaptionSession. The audio engine thread only calls pushProgramAudio(),
// which copies into a bounded drop-oldest ring and returns; everything else (resampling, VAD,
// inference, publishing) runs on one dedicated worker thread. If the recogniser cannot keep up the
// OLDEST audio is dropped (captions skip ahead) - the audio engine and video clock never wait.
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "captions/audio_tap.hpp"
#include "captions/session.hpp"

namespace hb::captions {

struct ServiceStatus {
    enum class State { Stopped, Loading, Listening, Error };
    State state = State::Stopped;
    std::int64_t latencyMs = 0;     // decode time + backlog of the last recognition
    std::int64_t decodeMs = 0;
    std::uint64_t droppedMs = 0;    // audio discarded because the worker was behind
    std::uint64_t finals = 0;
    std::uint64_t errors = 0;
    std::string error;              // load error, or the last recogniser error
    std::string engine;
};

const char* stateName(ServiceStatus::State s);

class CaptionService {
public:
    CaptionService(std::unique_ptr<ISpeechRecognizer> recognizer, hb::core::EventBus* bus, SessionConfig cfg,
                   std::size_t queueSeconds = 8);
    ~CaptionService();
    CaptionService(const CaptionService&) = delete;
    CaptionService& operator=(const CaptionService&) = delete;

    void start();   // returns immediately; model loading happens on the worker (state Loading)
    void stop();    // joins the worker; idempotent
    // Engine thread: copies stereo 48 kHz float frames; never blocks on the worker.
    void pushProgramAudio(const float* stereo48k, std::size_t frames) noexcept;
    ServiceStatus status() const;
    // SRT of the committed captions so far (valid after stop() or while running).
    std::string srtText() const;
    std::size_t srtCueCount() const;

private:
    void run();
    void setState(ServiceStatus::State s, const std::string& err = {});

    std::unique_ptr<ISpeechRecognizer> rec_;
    hb::core::EventBus* bus_;
    SessionConfig cfg_;
    AudioTapQueue queue_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> accepting_{false};
    mutable std::mutex mu_;
    ServiceStatus status_;
    std::string srt_;
    std::size_t cues_ = 0;
};

}  // namespace hb::captions
