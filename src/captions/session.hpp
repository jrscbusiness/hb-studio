#pragma once
// Synchronous caption pipeline core: mono 16 kHz samples -> segmenter -> recogniser -> committer ->
// normaliser -> EventBus "caption.text" {text, final:"1"|"0", lang} (+ SRT cues for finals).
// No threads here (CaptionService owns the worker); deterministic and unit-testable with
// FakeRecognizer and an injected clock. Never throws.
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "captions/committer.hpp"
#include "captions/recognizer.hpp"
#include "captions/segmenter.hpp"
#include "captions/srt.hpp"
#include "captions/text_normalizer.hpp"
#include "core/event_bus.hpp"

namespace hb::captions {

struct SessionConfig {
    std::string language = "pt";
    NormalizeOptions normalize;
    SegmenterConfig segmenter;
    bool partials = true;                  // publish interim (final="0") captions when the engine supports it
    std::int64_t maxBacklogForPartialsMs = 600;   // skip partial decoding while the worker is behind by more than this
};

struct SessionStats {
    std::uint64_t utterances = 0;
    std::uint64_t partialsPublished = 0;
    std::uint64_t finalsPublished = 0;
    std::uint64_t recognizerErrors = 0;
    std::uint64_t partialsSkipped = 0;
    std::int64_t lastDecodeMs = 0;
    std::int64_t lastLatencyMs = 0;        // last decode time + audio backlog at that moment
    std::string lastError;
};

class CaptionSession {
public:
    using NowMs = std::function<std::int64_t()>;
    CaptionSession(ISpeechRecognizer& recognizer, hb::core::EventBus* bus, SessionConfig cfg, NowMs nowMs);

    // backlogMs: audio still queued behind these samples (for the latency figure and partial throttling).
    void feed(const float* mono16k, std::size_t n, std::int64_t backlogMs = 0);
    void finish();   // flushes an open utterance (decoded as a final)

    SessionStats stats() const { return stats_; }
    const SrtBuilder& srt() const { return srt_; }
    std::int64_t positionMs() const { return seg_.positionMs(); }

private:
    void handle(const SegmentEvent& ev, std::int64_t backlogMs);
    void publish(const std::string& text, bool final);

    ISpeechRecognizer& rec_;
    hb::core::EventBus* bus_;
    SessionConfig cfg_;
    NowMs now_;
    Segmenter seg_;
    CaptionCommitter committer_;
    SrtBuilder srt_;
    SessionStats stats_;
    std::string lastPartialPublished_;
};

}  // namespace hb::captions
