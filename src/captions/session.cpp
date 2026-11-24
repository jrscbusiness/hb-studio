#include "captions/session.hpp"

#include <algorithm>
#include <exception>

namespace hb::captions {

CaptionSession::CaptionSession(ISpeechRecognizer& recognizer, hb::core::EventBus* bus, SessionConfig cfg, NowMs nowMs)
    : rec_(recognizer), bus_(bus), cfg_(std::move(cfg)), now_(std::move(nowMs)), seg_(cfg_.segmenter) {
    if (!now_) now_ = [] { return std::int64_t{0}; };
    cfg_.normalize.maxLines = std::max<std::size_t>(cfg_.normalize.maxLines, 1);
    cfg_.normalize.maxCharsPerLine = std::clamp<std::size_t>(cfg_.normalize.maxCharsPerLine, 10, 120);
}

void CaptionSession::feed(const float* mono16k, std::size_t n, std::int64_t backlogMs) {
    try {
        for (const auto& ev : seg_.feed(mono16k, n)) handle(ev, backlogMs);
    } catch (...) {
        ++stats_.recognizerErrors;
        stats_.lastError = "erro interno";
    }
}

void CaptionSession::finish() {
    try {
        for (const auto& ev : seg_.flush()) handle(ev, 0);
    } catch (...) {
        ++stats_.recognizerErrors;
    }
}

void CaptionSession::publish(const std::string& text, bool final) {
    if (!bus_) return;
    hb::core::Event e;
    e.topic = "caption.text";
    e.fields["text"] = text;
    e.fields["final"] = final ? "1" : "0";
    e.fields["lang"] = cfg_.language;
    bus_->publish(e);
    if (final) ++stats_.finalsPublished; else ++stats_.partialsPublished;
}

void CaptionSession::handle(const SegmentEvent& ev, std::int64_t backlogMs) {
    const bool isFinal = ev.kind == SegmentEvent::Kind::Final;
    if (!isFinal) {
        if (!cfg_.partials || !rec_.supportsPartials()) return;
        if (backlogMs > cfg_.maxBacklogForPartialsMs) { ++stats_.partialsSkipped; return; }   // catch up first
    } else {
        ++stats_.utterances;
    }
    RecognizeRequest req;
    req.pcm = ev.audio.data();
    req.samples = ev.audio.size();
    req.language = cfg_.language;
    req.partial = !isFinal;
    const std::int64_t t0 = now_();
    RecognizeResult res;
    try {
        res = rec_.recognize(req);
    } catch (const std::exception& e) {
        res.ok = false;
        res.error = std::string("exceção: ") + e.what();
    } catch (...) {
        res.ok = false;
        res.error = "exceção desconhecida";
    }
    const std::int64_t t1 = now_();
    stats_.lastDecodeMs = std::max<std::int64_t>(0, t1 - t0);
    stats_.lastLatencyMs = stats_.lastDecodeMs + std::max<std::int64_t>(0, backlogMs);
    if (!res.ok) {
        ++stats_.recognizerErrors;
        stats_.lastError = res.error;
        return;
    }
    stats_.lastError.clear();

    const std::string clean = cleanText(res.text);
    if (!isFinal) {
        const auto up = committer_.onPartial(ev.id, clean);
        if (!up.emit) return;
        const std::string norm = normalizeCaption(up.text, cfg_.normalize, false);
        const auto pages = paginate(norm, cfg_.normalize.maxCharsPerLine, cfg_.normalize.maxLines);
        if (pages.empty()) return;
        if (pages.back() == lastPartialPublished_) return;
        lastPartialPublished_ = pages.back();   // only the newest page of a long partial is shown
        publish(lastPartialPublished_, false);
        return;
    }

    const auto up = committer_.onFinal(ev.id, clean);
    const bool hadPartial = !lastPartialPublished_.empty();
    lastPartialPublished_.clear();
    if (!up.emit) return;
    if (up.text.empty()) {   // nothing to commit: tell consumers to drop the interim text
        if (hadPartial) publish("", true);
        return;
    }
    const std::string norm = normalizeCaption(up.text, cfg_.normalize, true);
    const auto pages = paginate(norm, cfg_.normalize.maxCharsPerLine, cfg_.normalize.maxLines);
    // SRT: split the utterance's time span over its pages proportionally to their length.
    std::size_t total = 0;
    for (const auto& p : pages) total += p.size();
    std::int64_t cursor = ev.startMs;
    const std::int64_t span = std::max<std::int64_t>(ev.endMs - ev.startMs, 1);
    std::size_t acc = 0;
    for (const auto& p : pages) {
        acc += p.size();
        const std::int64_t end = ev.startMs + span * static_cast<std::int64_t>(acc) / static_cast<std::int64_t>(std::max<std::size_t>(total, 1));
        srt_.add(cursor, end, p);
        cursor = end;
        publish(p, true);
    }
}

}  // namespace hb::captions
