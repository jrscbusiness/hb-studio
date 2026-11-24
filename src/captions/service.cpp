#include "captions/service.hpp"

#include <algorithm>
#include <chrono>
#include <exception>

namespace hb::captions {

const char* stateName(ServiceStatus::State s) {
    switch (s) {
        case ServiceStatus::State::Stopped: return "stopped";
        case ServiceStatus::State::Loading: return "loading";
        case ServiceStatus::State::Listening: return "listening";
        case ServiceStatus::State::Error: return "error";
    }
    return "stopped";
}

CaptionService::CaptionService(std::unique_ptr<ISpeechRecognizer> recognizer, hb::core::EventBus* bus, SessionConfig cfg,
                               std::size_t queueSeconds)
    : rec_(std::move(recognizer)), bus_(bus), cfg_(std::move(cfg)),
      queue_(static_cast<std::size_t>(kProgramRate) * std::max<std::size_t>(queueSeconds, 2)) {
    if (rec_) status_.engine = rec_->name();
}

CaptionService::~CaptionService() { stop(); }

void CaptionService::setState(ServiceStatus::State s, const std::string& err) {
    std::lock_guard lock(mu_);
    status_.state = s;
    if (!err.empty() || s == ServiceStatus::State::Error) status_.error = err;
}

void CaptionService::start() {
    if (thread_.joinable() || !rec_) return;
    stop_ = false;
    queue_.reopen();
    queue_.clear();
    { std::lock_guard lock(mu_); status_ = ServiceStatus{}; status_.engine = rec_->name(); status_.state = ServiceStatus::State::Loading; }
    thread_ = std::thread([this] { run(); });
}

void CaptionService::stop() {
    stop_ = true;
    accepting_ = false;
    queue_.close();
    if (thread_.joinable()) thread_.join();
    std::lock_guard lock(mu_);
    if (status_.state != ServiceStatus::State::Error) status_.state = ServiceStatus::State::Stopped;
}

void CaptionService::pushProgramAudio(const float* stereo48k, std::size_t frames) noexcept {
    if (!accepting_.load(std::memory_order_relaxed)) return;
    queue_.push(stereo48k, frames);
}

ServiceStatus CaptionService::status() const {
    std::lock_guard lock(mu_);
    ServiceStatus s = status_;
    s.droppedMs = queue_.droppedFrames() * 1000 / kProgramRate;
    return s;
}

std::string CaptionService::srtText() const {
    std::lock_guard lock(mu_);
    return srt_;
}

std::size_t CaptionService::srtCueCount() const {
    std::lock_guard lock(mu_);
    return cues_;
}

void CaptionService::run() {
    try {
        std::string err;
        if (!rec_->load(err)) {
            setState(ServiceStatus::State::Error, err.empty() ? "não foi possível iniciar o reconhecedor" : err);
            return;
        }
        if (stop_) return;
        const auto t0 = std::chrono::steady_clock::now();
        CaptionSession session(*rec_, bus_, cfg_, [t0] {
            return static_cast<std::int64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
        });
        ProgramDownmixer down;
        queue_.clear();   // audio that piled up while the model was loading is stale
        accepting_ = true;
        setState(ServiceStatus::State::Listening);

        std::vector<float> stereo(4800 * 2), mono;
        auto publishStatus = [&] {
            const auto st = session.stats();
            std::lock_guard lock(mu_);
            status_.latencyMs = st.lastLatencyMs;
            status_.decodeMs = st.lastDecodeMs;
            status_.finals = st.finalsPublished;
            status_.errors = st.recognizerErrors;
            if (!st.lastError.empty()) status_.error = st.lastError; else if (status_.state != ServiceStatus::State::Error) status_.error.clear();
            if (session.srt().size() != cues_) {   // rebuild the text only when a cue was added
                srt_ = session.srt().str();
                cues_ = session.srt().size();
            }
        };
        while (!stop_) {
            if (!queue_.waitForData(100)) continue;
            const std::size_t frames = queue_.pop(stereo.data(), 4800);
            if (frames == 0) continue;
            mono.clear();
            down.process(stereo.data(), frames, mono);
            const std::int64_t backlogMs = static_cast<std::int64_t>(queue_.sizeFrames()) * 1000 / kProgramRate;
            session.feed(mono.data(), mono.size(), backlogMs);
            publishStatus();
        }
        accepting_ = false;
        session.finish();
        publishStatus();
    } catch (const std::exception& e) {
        accepting_ = false;
        setState(ServiceStatus::State::Error, std::string("erro interno: ") + e.what());
    } catch (...) {
        accepting_ = false;
        setState(ServiceStatus::State::Error, "erro interno");
    }
}

}  // namespace hb::captions
