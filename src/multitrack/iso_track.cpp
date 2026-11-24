#include "multitrack/iso_track.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "recording/file_io.hpp"
#include "recording/wav_writer.hpp"

namespace hb::multitrack {

const char* trackStateName(TrackState s) {
    switch (s) {
        case TrackState::Idle: return "not-started";
        case TrackState::Recording: return "recording";
        case TrackState::Finished: return "finished";
        case TrackState::Error: return "error";
        case TrackState::DiskStopped: return "disk-stopped";
    }
    return "?";
}

// ------------------------------------------------------------------------------------------------ base
IsoTrack::IsoTrack(TrackSpec spec, TrackServices services) : spec_(std::move(spec)), svc_(services) {
    userRotation_ = spec_.rotationSeconds > 0;
}

IsoTrack::~IsoTrack() = default;   // subclasses call stop() in their destructors (the worker uses their members)

void IsoTrack::log(core::Level l, const std::string& m) {
    if (svc_.log) svc_.log->log(l, "multitrack", spec_.id + ": " + m);
}

std::string IsoTrack::segmentPath(int index) const {
    std::string name = spec_.stem;
    if (index > 1 || userRotation_) {
        char b[24];
        std::snprintf(b, sizeof b, "_%03d", index);
        name += b;
    }
    if (!spec_.profile.extension.empty()) name += "." + spec_.profile.extension;
    return rec::detail::pathToUtf8(rec::detail::pathFromUtf8(spec_.folder) / rec::detail::pathFromUtf8(name));
}

std::string IsoTrack::relPath(int index) const {
    const std::string full = segmentPath(index);
    const auto p = full.find_last_of("/\\");
    const std::string leaf = p == std::string::npos ? full : full.substr(p + 1);
    return spec_.relDir.empty() ? leaf : spec_.relDir + "/" + leaf;
}

bool IsoTrack::openSegment(std::uint64_t startUnit, Ns startNs, std::string& error) {
    const int idx = segment_ + 1;
    const std::string path = segmentPath(idx);
    try {
        writer_ = spec_.factory ? spec_.factory() : nullptr;
        if (!writer_) {
            error = "nao foi possivel criar o escritor do formato";
            return false;
        }
        if (!writer_->open(path, spec_.profile, spec_.kind == TrackKind::Video ? spec_.video : rec::VideoFormat{},
                           spec_.kind == TrackKind::Audio ? spec_.audio : rec::AudioFormat{}, error)) {
            if (error.empty()) error = "falha ao abrir o arquivo";
            writer_.reset();
            return false;
        }
    } catch (const std::exception& e) {
        error = std::string("excecao no escritor: ") + e.what();
        writer_.reset();
        return false;
    } catch (...) {
        error = "excecao no escritor";
        writer_.reset();
        return false;
    }
    segment_ = idx;
    ManifestSegment s;
    s.index = idx;
    s.file = relPath(idx);
    s.startNs = startNs;
    s.startUnit = startUnit;
    std::lock_guard lk(mu_);
    segs_.push_back(s);
    return true;
}

void IsoTrack::closeSegment() {
    if (!writer_) return;
    bool ok = false;
    std::uint64_t bytes = 0;
    try {
        std::string e;
        ok = writer_->close(e);
        bytes = writer_->stats().bytesWritten;
        if (!ok) log(core::Level::Error, "falha ao finalizar o arquivo: " + e);
    } catch (...) {
        log(core::Level::Error, "excecao ao finalizar o arquivo");
    }
    writer_.reset();
    std::string path;
    int idx;
    {
        std::lock_guard lk(mu_);
        if (segs_.empty()) return;
        segs_.back().bytes = bytes;
        segs_.back().closed = ok;
        idx = segs_.back().index;
    }
    path = segmentPath(idx);
    if (ok && svc_.bus) {
        try {
            svc_.bus->publish(core::Event{"recording.fileClosed",
                                          {{"path", path},
                                           {"kind", spec_.kind == TrackKind::Video ? "iso-video" : "iso-audio"},
                                           {"index", std::to_string(idx)},
                                           {"track", spec_.id}}});
        } catch (...) {
        }
    }
}

void IsoTrack::failTrack(const std::string& message) {
    closeSegment();   // best effort; announces the file only if it closed cleanly
    {
        std::lock_guard lk(mu_);
        error_ = message;
    }
    log(core::Level::Error, "trilha interrompida: " + message);
    state_ = static_cast<int>(TrackState::Error);
}

bool IsoTrack::syncWavHeader(std::string& error) {
    auto* w = dynamic_cast<rec::WavWriter*>(writer_.get());
    return w ? w->syncHeader(error) : true;
}

bool IsoTrack::start(std::string& error) {
    if (state_.load() != static_cast<int>(TrackState::Idle)) {
        error = "trilha ja iniciada";
        return false;
    }
    if (!openSegment(0, 0, error)) {
        {
            std::lock_guard lk(mu_);
            error_ = error;
        }
        state_ = static_cast<int>(TrackState::Error);
        return false;
    }
    state_ = static_cast<int>(TrackState::Recording);
    thread_ = std::thread([this] {
        try {
            workerLoop();
        } catch (...) {
            failTrack("erro interno na trilha");
        }
        closeSegment();
        if (state_.load() == static_cast<int>(TrackState::Recording))
            state_ = static_cast<int>(diskStop_.load() ? TrackState::DiskStopped : TrackState::Finished);
    });
    return true;
}

void IsoTrack::stop() {
    stopReq_ = true;
    cv_.notify_all();
    if (thread_.joinable() && thread_.get_id() != std::this_thread::get_id()) thread_.join();
}

void IsoTrack::stopForDisk(const std::string& reason) {
    {
        std::lock_guard lk(mu_);
        if (error_.empty()) error_ = reason;
    }
    diskStop_ = true;
    stop();
}

TrackStatus IsoTrack::status() const {
    TrackStatus s;
    s.id = spec_.id;
    s.name = spec_.name;
    s.kind = spec_.kind;
    s.state = static_cast<TrackState>(state_.load());
    {
        std::lock_guard lk(mu_);
        s.error = error_;
        s.segment = segs_.empty() ? 0 : segs_.back().index;
        if (!segs_.empty()) s.currentFile = segs_.back().file;
        for (const auto& g : segs_) s.bytes += g.bytes;
    }
    fillStatus(s);
    return s;
}

ManifestTrack IsoTrack::manifest() const {
    ManifestTrack t;
    t.id = spec_.id;
    t.kind = spec_.kind == TrackKind::Video ? "video" : "audio";
    t.name = spec_.name;
    t.device = spec_.device;
    t.codec = spec_.codec;
    t.sourceIndex = spec_.sourceIndex;
    t.tap = spec_.tap;
    t.state = trackStateName(static_cast<TrackState>(state_.load()));
    {
        std::lock_guard lk(mu_);
        t.error = error_;
        t.segments = segs_;
    }
    fillManifest(t);
    return t;
}

// ------------------------------------------------------------------------------------------------ video
IsoVideoTrack::IsoVideoTrack(TrackSpec spec, TrackServices services)
    : IsoTrack(std::move(spec), services),
      tl_(VideoTimelineConfig{spec_.video.fpsNum, spec_.video.fpsDen, 0}) {}

void IsoVideoTrack::pushVideo(std::shared_ptr<const video::Frame> frame, Ns captureNs) {
    if (!frame || state_.load() != static_cast<int>(TrackState::Recording) || stopReq_.load()) return;
    {
        std::lock_guard lk(mu_);
        const std::size_t depth = static_cast<std::size_t>(std::max(1, spec_.queueDepth));
        while (q_.size() >= depth) {
            q_.pop_front();
            ++droppedQueue_;
        }
        q_.push_back({std::move(frame), captureNs});
    }
    cv_.notify_one();
}

bool IsoVideoTrack::writeSlot(const video::Frame& f, std::uint64_t slot) {
    const Ns rotNs = static_cast<Ns>(spec_.rotationSeconds) * kNsPerSec;
    if (spec_.rotationSeconds > 0 && slot >= segEndSlot_) {
        {
            std::lock_guard lk(mu_);
            if (!segs_.empty()) segs_.back().units = segUnits_;
        }
        closeSegment();
        std::string e;
        if (!openSegment(slot, tl_.slotNs(slot), e)) {
            failTrack(e);
            return false;
        }
        segBaseSlot_ = slot;
        segUnits_ = 0;
        segEndSlot_ = tl_.slotAt((tl_.slotNs(slot) / rotNs + 1) * rotNs);
    }
    std::string e;
    bool ok = false;
    try {
        ok = writer_ && writer_->writeVideo(f.data.data(), f.stride, tl_.slotNs(slot) - tl_.slotNs(segBaseSlot_), e);
    } catch (...) {
        e = "excecao no escritor";
    }
    if (!ok) {
        failTrack(e.empty() ? "falha ao gravar o video" : e);
        return false;
    }
    ++segUnits_;
    ++units_;
    std::lock_guard lk(mu_);
    if (!segs_.empty()) segs_.back().units = segUnits_;
    return true;
}

void IsoVideoTrack::workerLoop() {
    for (;;) {
        Item it;
        {
            std::unique_lock lk(mu_);
            cv_.wait(lk, [&] { return !q_.empty() || stopReq_.load(); });
            if (q_.empty()) break;
            it = std::move(q_.front());
            q_.pop_front();
        }
        if (state_.load() != static_cast<int>(TrackState::Recording)) continue;
        const video::Frame& f = *it.frame;
        if (f.width != spec_.video.width || f.height != spec_.video.height || f.stride < f.width * 4 ||
            f.data.size() < static_cast<std::size_t>(f.stride) * static_cast<std::size_t>(f.height)) {
            ++droppedSize_;
            continue;
        }
        const VideoPlacement p = tl_.onFrame(it.ns);
        if (p.drop) {
            ++droppedTl_;
            continue;
        }
        if (!segBaseSet_) {
            segBaseSet_ = true;
            segBaseSlot_ = p.slot;
            firstOffsetNs_ = tl_.slotNs(p.slot);
            if (spec_.rotationSeconds > 0) {
                const Ns rotNs = static_cast<Ns>(spec_.rotationSeconds) * kNsPerSec;
                segEndSlot_ = tl_.slotAt((tl_.slotNs(p.slot) / rotNs + 1) * rotNs);
            }
            std::lock_guard lk(mu_);
            if (!segs_.empty()) {
                segs_.back().startNs = firstOffsetNs_;
                segs_.back().startUnit = p.slot;
            }
        }
        if (p.repeatCount && prev_) {
            for (std::uint64_t i = 0; i < p.repeatCount; ++i)
                if (!writeSlot(*prev_, p.repeatFrom + i)) break;
            repeated_ = tl_.repeated();
            jumped_ = tl_.jumped();
        }
        if (state_.load() != static_cast<int>(TrackState::Recording)) continue;
        if (!writeSlot(f, p.slot)) continue;
        prev_ = it.frame;
    }
}

void IsoVideoTrack::fillStatus(TrackStatus& s) const {
    s.units = units_.load();
    s.repeated = repeated_.load();
    s.droppedQueue = droppedQueue_.load() + droppedSize_.load();
    s.droppedTimeline = droppedTl_.load();
    s.jumped = jumped_.load();
    const double fps = spec_.video.fpsDen > 0 ? static_cast<double>(spec_.video.fpsNum) / spec_.video.fpsDen : 30.0;
    s.seconds = static_cast<double>(s.units) / fps;
}

void IsoVideoTrack::fillManifest(ManifestTrack& t) const {
    t.width = spec_.video.width;
    t.height = spec_.video.height;
    t.fpsNum = spec_.video.fpsNum;
    t.fpsDen = spec_.video.fpsDen;
    t.units = units_.load();
    t.repeated = repeated_.load();
    t.droppedQueue = droppedQueue_.load() + droppedSize_.load();
    t.droppedTimeline = droppedTl_.load();
    t.jumped = jumped_.load();
    t.startOffsetNs = t.segments.empty() ? 0 : t.segments.front().startNs;
}

// ------------------------------------------------------------------------------------------------ audio
IsoAudioTrack::IsoAudioTrack(TrackSpec spec, TrackServices services)
    : IsoTrack(std::move(spec), services),
      tl_(AudioTimelineConfig{std::max(1, spec_.audio.sampleRate)}) {
    const std::uint64_t rate = static_cast<std::uint64_t>(std::max(1, spec_.audio.sampleRate));
    const std::uint64_t ch = static_cast<std::uint64_t>(std::max(1, spec_.audio.channels));
    if (spec_.rotationSeconds > 0) rotFrames_ = static_cast<std::uint64_t>(spec_.rotationSeconds) * rate;
    if (spec_.profile.id.rfind("wav", 0) == 0) {   // 4 GB RIFF limit: roll over before it
        const std::uint64_t bpf = ch * static_cast<std::uint64_t>(rec::detail::normalizeBits(spec_.profile.audioBitsPerSample) / 8);
        const std::uint64_t cap = 3500000000ull / std::max<std::uint64_t>(1, bpf);
        rotFrames_ = rotFrames_ ? std::min(rotFrames_, cap) : cap;
    }
    segEnd_ = rotFrames_;
    zeros_.assign(4800 * ch, 0.0f);
    last_.assign(ch, 0.0f);
}

void IsoAudioTrack::pushAudio(const float* in, std::size_t frames, Ns startNs) {
    if (!in || frames == 0 || state_.load() != static_cast<int>(TrackState::Recording) || stopReq_.load()) return;
    const std::size_t ch = static_cast<std::size_t>(std::max(1, spec_.audio.channels));
    {
        std::lock_guard lk(mu_);
        const std::uint64_t maxFrames = static_cast<std::uint64_t>(spec_.maxQueuedSeconds * spec_.audio.sampleRate);
        while (!q_.empty() && queuedFrames_ + frames > maxFrames) {
            const std::uint64_t carry = q_.front().lostBefore + q_.front().frames;
            queuedFrames_ -= q_.front().frames;
            droppedQueue_ += q_.front().frames;
            q_.pop_front();
            if (q_.empty()) pendingLost_ += carry;
            else q_.front().lostBefore += carry;
        }
        Block b;
        b.data.assign(in, in + frames * ch);
        b.frames = frames;
        b.startNs = startNs;
        b.lostBefore = pendingLost_;
        pendingLost_ = 0;
        queuedFrames_ += frames;
        q_.push_back(std::move(b));
    }
    cv_.notify_one();
}

bool IsoAudioTrack::writeFrames(const float* data, std::uint64_t n, bool repeatLast) {
    const std::size_t ch = static_cast<std::size_t>(std::max(1, spec_.audio.channels));
    const std::uint64_t rate = static_cast<std::uint64_t>(std::max(1, spec_.audio.sampleRate));
    std::vector<float> rep;
    while (n > 0) {
        if (rotFrames_ && pos_ >= segEnd_) {
            {
                std::lock_guard lk(mu_);
                if (!segs_.empty()) segs_.back().units = segUnits_;
            }
            closeSegment();
            std::string e;
            if (!openSegment(pos_, unitsToNs(pos_, rate, 1), e)) {
                failTrack(e);
                return false;
            }
            segBase_ = pos_;
            segUnits_ = 0;
            sinceSync_ = 0;
            segEnd_ = (pos_ / rotFrames_ + 1) * rotFrames_;
        }
        std::uint64_t chunk = std::min<std::uint64_t>(n, 4800);
        if (rotFrames_) chunk = std::min(chunk, segEnd_ - pos_);
        const float* src = data;
        if (!data) {
            src = zeros_.data();
        } else if (repeatLast) {
            rep.resize(static_cast<std::size_t>(chunk) * ch);
            for (std::uint64_t i = 0; i < chunk; ++i) std::memcpy(&rep[static_cast<std::size_t>(i) * ch], last_.data(), ch * sizeof(float));
            src = rep.data();
        }
        std::string e;
        bool ok = false;
        try {
            ok = writer_ && writer_->writeAudio(src, static_cast<std::size_t>(chunk), unitsToNs(pos_ - segBase_, rate, 1), e);
        } catch (...) {
            e = "excecao no escritor";
        }
        if (!ok) {
            failTrack(e.empty() ? "falha ao gravar o audio" : e);
            return false;
        }
        if (!data) std::fill(last_.begin(), last_.end(), 0.0f);
        else if (!repeatLast) std::memcpy(last_.data(), src + (static_cast<std::size_t>(chunk) - 1) * ch, ch * sizeof(float));
        pos_ += chunk;
        segUnits_ += chunk;
        units_ += chunk;
        sinceSync_ += chunk;
        if (data && !repeatLast) data += static_cast<std::size_t>(chunk) * ch;
        n -= chunk;
        if (spec_.wavSyncSeconds > 0 && sinceSync_ >= rate * static_cast<std::uint64_t>(spec_.wavSyncSeconds)) {
            sinceSync_ = 0;
            std::string se;
            syncWavHeader(se);   // best effort: a failed refresh only affects crash recovery
        }
        std::lock_guard lk(mu_);
        if (!segs_.empty()) segs_.back().units = segUnits_;
    }
    return true;
}

void IsoAudioTrack::workerLoop() {
    const std::size_t ch = static_cast<std::size_t>(std::max(1, spec_.audio.channels));
    const std::uint64_t rate = static_cast<std::uint64_t>(std::max(1, spec_.audio.sampleRate));
    for (;;) {
        Block b;
        {
            std::unique_lock lk(mu_);
            cv_.wait(lk, [&] { return !q_.empty() || stopReq_.load(); });
            if (q_.empty()) break;
            b = std::move(q_.front());
            queuedFrames_ -= b.frames;
            q_.pop_front();
        }
        if (state_.load() != static_cast<int>(TrackState::Recording)) continue;
        if (b.lostBefore && pos_ > 0) {
            tl_.addExternalGap(b.lostBefore);
            if (!writeFrames(nullptr, b.lostBefore, false)) continue;
        }
        const AudioPlacement p = tl_.onBlock(b.startNs, b.frames);
        bool ok = true;
        if (p.silenceBefore) ok = writeFrames(nullptr, p.silenceBefore, false);
        if (ok && p.repeatLast) ok = writeFrames(last_.data(), p.repeatLast, true);
        if (ok && p.skipFrames < b.frames)
            ok = writeFrames(b.data.data() + static_cast<std::size_t>(p.skipFrames) * ch, b.frames - p.skipFrames, false);
        silence_ = tl_.insertedSilence();
        skipped_ = tl_.skippedFrames();
        repeated_ = tl_.repeatedFrames();
        leading_ = tl_.leadingSilence();
        const Ns el = b.startNs + unitsToNs(b.frames, rate, 1);
        elapsedNs_ = el;
        drift_ = tl_.driftPpm(el);
        if (!ok) continue;
    }
}

void IsoAudioTrack::fillStatus(TrackStatus& s) const {
    s.units = units_.load();
    s.repeated = repeated_.load();
    s.droppedQueue = droppedQueue_.load();
    s.droppedTimeline = skipped_.load();
    s.silence = silence_.load() + leading_.load();
    s.driftPpm = drift_.load();
    s.seconds = static_cast<double>(s.units) / std::max(1, spec_.audio.sampleRate);
}

void IsoAudioTrack::fillManifest(ManifestTrack& t) const {
    t.sampleRate = spec_.audio.sampleRate;
    t.channels = spec_.audio.channels;
    t.bitsPerSample = rec::detail::normalizeBits(spec_.profile.audioBitsPerSample);
    t.units = units_.load();
    t.repeated = repeated_.load();
    t.droppedQueue = droppedQueue_.load();
    t.droppedTimeline = skipped_.load();
    t.silenceFrames = silence_.load();
    t.leadingSilenceFrames = leading_.load();
    t.driftPpm = drift_.load();
    t.startOffsetNs = 0;   // sample 0 of the first file IS the session start (leading silence padded)
}

}  // namespace hb::multitrack
