#include "captions/segmenter.hpp"

#include <algorithm>
#include <cmath>

namespace hb::captions {

Segmenter::Segmenter(SegmenterConfig cfg) : cfg_(cfg) {
    if (cfg_.sampleRate < 8000) cfg_.sampleRate = 16000;
    cfg_.frameMs = std::clamp(cfg_.frameMs, 5, 100);
    frameSamples_ = static_cast<std::size_t>(cfg_.sampleRate) * static_cast<std::size_t>(cfg_.frameMs) / 1000;
    noiseDb_ = cfg_.minThresholdDb - cfg_.marginDb;
}

std::vector<SegmentEvent> Segmenter::feed(const float* samples, std::size_t n) {
    std::vector<SegmentEvent> out;
    if (!samples || n == 0) return out;
    pending_.insert(pending_.end(), samples, samples + n);
    std::size_t pos = 0;
    while (pending_.size() - pos >= frameSamples_) {
        processFrame(pending_.data() + pos, out);
        pos += frameSamples_;
    }
    pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(pos));
    return out;
}

void Segmenter::processFrame(const float* f, std::vector<SegmentEvent>& out) {
    double sum = 0;
    for (std::size_t i = 0; i < frameSamples_; ++i) sum += static_cast<double>(f[i]) * f[i];
    const double rms = std::sqrt(sum / static_cast<double>(frameSamples_));
    const double db = rms > 1e-9 ? 20.0 * std::log10(rms) : -180.0;
    if (!noiseInit_) { noiseDb_ = std::min(db, cfg_.minThresholdDb); noiseInit_ = true; }
    // Minimum statistics over the window: raises the floor for steady noise, never for speech (which pauses).
    {
        const std::uint64_t idx = frameIdx_++;
        while (!minWin_.empty() && minWin_.back().second >= db) minWin_.pop_back();
        minWin_.emplace_back(idx, db);
        const std::uint64_t win = static_cast<std::uint64_t>(std::max(cfg_.noiseWindowMs / cfg_.frameMs, 1));
        while (minWin_.front().first + win <= idx) minWin_.pop_front();
        if (idx + 1 >= win && minWin_.front().second > noiseDb_) noiseDb_ = minWin_.front().second;
    }
    const double threshold = std::max(cfg_.minThresholdDb, noiseDb_ + cfg_.marginDb);
    const bool voiced = db > threshold;
    const std::int64_t frameStartMs = samplesToMs(totalSamples_);
    totalSamples_ += frameSamples_;
    const std::int64_t frameEndMs = samplesToMs(totalSamples_);

    if (!inSpeech_) {
        // Adapt the noise floor only while idle (slowly up, a bit faster down) so speech never raises it.
        if (!voiced) noiseDb_ += (db - noiseDb_) * (db < noiseDb_ ? 0.10 : 0.02);
        recent_.emplace_back(f, f + frameSamples_);
        const std::size_t keep = static_cast<std::size_t>((cfg_.preRollMs + cfg_.minSpeechMs) / cfg_.frameMs) + 2;
        while (recent_.size() > keep) recent_.pop_front();
        voicedRun_ = voiced ? voicedRun_ + 1 : 0;
        if (voicedRun_ * cfg_.frameMs >= cfg_.minSpeechMs) {
            inSpeech_ = true;
            ++utterId_;
            utter_.clear();
            // pre-roll: the voiced run plus preRollMs before it
            const std::size_t want = static_cast<std::size_t>(voicedRun_ + cfg_.preRollMs / cfg_.frameMs);
            const std::size_t take = std::min(want, recent_.size());
            for (std::size_t i = recent_.size() - take; i < recent_.size(); ++i)
                utter_.insert(utter_.end(), recent_[i].begin(), recent_[i].end());
            utterStartMs_ = frameEndMs - static_cast<std::int64_t>(take) * cfg_.frameMs;
            recent_.clear();
            voicedRun_ = 0;
            silenceMs_ = 0;
            lastVoiceEndMs_ = frameEndMs;
            lastVoiceEndSample_ = utter_.size();
            lastPartialAtMs_ = 0;
        }
        return;
    }

    utter_.insert(utter_.end(), f, f + frameSamples_);
    if (voiced) {
        silenceMs_ = 0;
        lastVoiceEndMs_ = frameEndMs;
        lastVoiceEndSample_ = utter_.size();
    } else {
        silenceMs_ += cfg_.frameMs;
    }
    (void)frameStartMs;
    const std::int64_t lenMs = frameEndMs - utterStartMs_;

    if (silenceMs_ >= cfg_.hangoverMs) { closeUtterance(out, true); return; }
    if (lenMs >= cfg_.softMaxMs && silenceMs_ >= cfg_.softCutSilenceMs) { closeUtterance(out, true); return; }
    if (lenMs >= cfg_.maxUtteranceMs) { closeUtterance(out, false); return; }

    if (cfg_.partialIntervalMs > 0 && lenMs >= cfg_.minPartialMs && lenMs - lastPartialAtMs_ >= cfg_.partialIntervalMs) {
        lastPartialAtMs_ = lenMs;
        SegmentEvent ev;
        ev.kind = SegmentEvent::Kind::Partial;
        ev.id = utterId_;
        ev.startMs = utterStartMs_;
        ev.endMs = frameEndMs;
        ev.audio = utter_;
        out.push_back(std::move(ev));
    }
}

void Segmenter::closeUtterance(std::vector<SegmentEvent>& out, bool endAtLastVoice) {
    SegmentEvent ev;
    ev.kind = SegmentEvent::Kind::Final;
    ev.id = utterId_;
    ev.startMs = utterStartMs_;
    std::size_t keepSamples = utter_.size();
    if (endAtLastVoice) {
        const std::size_t tail = static_cast<std::size_t>(cfg_.sampleRate) * static_cast<std::size_t>(cfg_.trailingKeepMs) / 1000;
        keepSamples = std::min(utter_.size(), lastVoiceEndSample_ + tail);
    }
    utter_.resize(keepSamples);
    ev.audio = std::move(utter_);
    ev.endMs = ev.startMs + static_cast<std::int64_t>(ev.audio.size()) * 1000 / cfg_.sampleRate;
    utter_.clear();
    out.push_back(std::move(ev));
    inSpeech_ = false;
    silenceMs_ = 0;
    voicedRun_ = 0;
    recent_.clear();
}

std::vector<SegmentEvent> Segmenter::flush() {
    std::vector<SegmentEvent> out;
    if (inSpeech_) closeUtterance(out, true);
    pending_.clear();
    return out;
}

}  // namespace hb::captions
