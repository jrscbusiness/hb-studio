#include "recording/recorder.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>

#include "recording/file_io.hpp"
#include "recording/file_naming.hpp"

namespace hb::rec {

namespace {

enum StateInt { kIdle = 0, kRecording = 1, kStopping = 2, kError = 3 };

constexpr std::size_t kChunkFrames = 8192;

// floor(count * a / b * 1e9) without overflow for realistic values.
std::int64_t ratioNs(std::uint64_t count, std::uint64_t a, std::uint64_t b) {
    const std::uint64_t x = count * a;
    const std::uint64_t q = x / b, r = x % b;
    return static_cast<std::int64_t>(q * 1000000000ull + r * 1000000000ull / b);
}

std::string baseName(const std::string& pathUtf8) {
    return detail::pathToUtf8(detail::pathFromUtf8(pathUtf8).filename());
}

std::string mb(std::uint64_t bytes) { return std::to_string(bytes >> 20) + " MB"; }

}  // namespace

Recorder::Recorder(RecorderConfig config, core::EventBus* bus, core::Logger* log)
    : cfg_(std::move(config)), bus_(bus), logger_(log) {
    if (!cfg_.nowNs)
        cfg_.nowNs = [] {
            return static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                 std::chrono::steady_clock::now().time_since_epoch())
                                                 .count());
        };
    if (!cfg_.freeSpaceBytes)
        cfg_.freeSpaceBytes = [](const std::string& folder) -> std::uint64_t {
            std::error_code ec;
            const auto sp = std::filesystem::space(detail::pathFromUtf8(folder), ec);
            return ec ? ~0ull : static_cast<std::uint64_t>(sp.available);  // unknown: do not raise a false alarm
        };
    cfg_.videoQueueFrames = std::max(1, cfg_.videoQueueFrames);
}

Recorder::~Recorder() { stop(); }

void Recorder::log(core::Level level, const std::string& message) {
    if (logger_) logger_->log(level, "recording", message);
}

void Recorder::publish(const std::string& topic, core::Fields fields) {
    if (!bus_) return;
    try {
        bus_->publish(core::Event{topic, std::move(fields)});
    } catch (...) {
    }
}

Recorder::Status Recorder::status() const {
    std::lock_guard lk(statusMu_);
    Status s = status_;
    s.droppedVideo = droppedVideo_.load();  // live: the writer thread may be busy or blocked
    s.droppedAudio = droppedAudio_.load();
    switch (state_.load()) {
        case kRecording: s.state = Status::State::Recording; break;
        case kStopping: s.state = Status::State::Stopping; break;
        case kError: s.state = Status::State::Error; break;
        default: s.state = Status::State::Idle; break;
    }
    return s;
}

void Recorder::updateStatus() {
    std::lock_guard lk(statusMu_);
    status_.videoFrames = slots_;
    status_.repeatedVideo = repeated_;
    status_.droppedVideo = droppedVideo_.load();
    status_.audioFrames = audioWritten_;
    status_.droppedAudio = droppedAudio_.load();
    status_.silenceFrames = silence_;
    // ---- FEATURE: multitrack ----
    if (haveT0_) status_.t0Ns = t0Ns_;
    // ---- /FEATURE ----
    status_.segment = segment_;
    status_.bytes = bytesDone_ + (writer_ ? writer_->stats().bytesWritten : 0);
    if (hasVideo_)
        status_.seconds = static_cast<double>(slots_) * cfg_.video.fpsDen / cfg_.video.fpsNum;
    else if (rate_ > 0)
        status_.seconds = static_cast<double>(audioWritten_) / rate_;
}

std::string Recorder::segmentPath(int segment) {
    if (segment <= 1) return uniqueOutputPath(folder_, baseName_, profile_.extension);
    char suffix[24];
    std::snprintf(suffix, sizeof(suffix), "_part%03d", segment);
    return uniqueOutputPath(folder_, baseName_ + suffix, profile_.extension);
}

bool Recorder::openSegment(std::string& error) {
    const int seg = segment_ + 1;
    const std::string path = segmentPath(seg);
    try {
        writer_ = factory_ ? factory_() : nullptr;
        if (!writer_) {
            error = "nao foi possivel criar o escritor do formato";
            return false;
        }
        const VideoFormat v = hasVideo_ ? cfg_.video : VideoFormat{};
        AudioFormat a;
        if (hasAudio_) a = AudioFormat{rate_, channels_};
        if (!writer_->open(path, profile_, v, a, error)) {
            if (error.empty()) error = "falha ao abrir o arquivo de saida";
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
    segment_ = seg;
    {
        std::lock_guard lk(statusMu_);
        status_.file = path;
    }
    return true;
}

void Recorder::closeWriter(std::string& error) {
    if (!writer_) return;
    bool closedOk = false;   // FEATURE: multitrack
    try {
        std::string e;
        if (!writer_->close(e)) error = e.empty() ? "falha ao finalizar o arquivo" : e;
        else closedOk = true;
        bytesDone_ += writer_->stats().bytesWritten;
    } catch (...) {
        error = "excecao ao finalizar o arquivo";
    }
    writer_.reset();
    // ---- FEATURE: multitrack ----
    // Frozen contract (cloud backup consumes it): every finalised recording file is announced, rotated segments included.
    if (closedOk) {
        const std::string closedPath = status().file;
        publish("recording.fileClosed", {{"path", closedPath}, {"kind", "program"}, {"index", std::to_string(segment_)}});
    }
    // ---- /FEATURE ----
}

bool Recorder::start(std::string& error) {
    std::lock_guard ctl(ctlMu_);
    const int st = state_.load();
    if (st == kRecording || st == kStopping) {
        error = "gravacao ja em andamento";
        return false;
    }
    if (thread_.joinable()) thread_.join();

    const auto failStart = [&](const std::string& msg) {
        error = msg;
        {
            std::lock_guard lk(statusMu_);
            status_.error = msg;
        }
        log(core::Level::Error, "Nao foi possivel iniciar a gravacao: " + msg);
        publish("recording.error", {{"message", msg}});   // published before the state change is visible (see fail())
        state_ = kError;
        return false;
    };

    // ---- resolve the format (only through the registry) ----
    RecordingProfile reg;
    WriterFactory factory;
    if (!WriterRegistry::instance().find(cfg_.profileId, reg, factory))
        return failStart("Formato de gravação desconhecido: " + cfg_.profileId);
    profile_ = reg;
    if (!cfg_.profile.id.empty()) {
        profile_.videoBitrateKbps = cfg_.profile.videoBitrateKbps;
        profile_.videoQuality = cfg_.profile.videoQuality;
        profile_.audioBitrateKbps = cfg_.profile.audioBitrateKbps;
        profile_.audioSampleRate = cfg_.profile.audioSampleRate;
        profile_.audioBitsPerSample = cfg_.profile.audioBitsPerSample;
        profile_.keyframeIntervalSec = cfg_.profile.keyframeIntervalSec;
    }
    factory_ = factory;
    hasVideo_ = profile_.hasVideo;
    hasAudio_ = profile_.hasAudio;
    if (hasVideo_ && (!cfg_.video.present() || cfg_.video.fpsNum <= 0 || cfg_.video.fpsDen <= 0))
        return failStart("O formato escolhido exige vídeo, mas o formato de vídeo não foi informado");
    if (hasAudio_ && !cfg_.audio.present())
        return failStart("O formato escolhido exige áudio, mas o formato de áudio não foi informado");
    if (!hasVideo_ && !hasAudio_) return failStart("Formato sem vídeo nem áudio");

    channels_ = hasAudio_ ? cfg_.audio.channels : 0;
    inRate_ = hasAudio_ ? cfg_.audio.sampleRate : 0;
    rate_ = hasAudio_ ? (profile_.audioSampleRate > 0 ? profile_.audioSampleRate : inRate_) : 0;
    resampler_.reset();
    if (hasAudio_ && inRate_ != rate_) {
        if (channels_ > 8) return failStart("Conversão de taxa de amostragem suporta no máximo 8 canais");
        resampler_ = std::make_unique<audio::Resampler>(inRate_, rate_, channels_, audio::ResamplerQuality::High,
                                                        4096);
    }

    // ---- folder, disk, names ----
    folder_ = cfg_.folderUtf8;
    {
        std::error_code ec;
        std::filesystem::create_directories(detail::pathFromUtf8(folder_), ec);
        if (ec) return failStart("Não foi possível criar a pasta de gravação");
    }
    const std::uint64_t freeNow = cfg_.freeSpaceBytes(folder_);
    if (freeNow < cfg_.minFreeBytes)
        return failStart("Disco cheio: restam " + mb(freeNow) + " livres (mínimo " + mb(cfg_.minFreeBytes) + ")");

    {
        const std::time_t tt = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        std::tm tm{};
#ifdef _WIN32
        localtime_s(&tm, &tt);
#else
        localtime_r(&tt, &tm);
#endif
        baseName_ = firstFreeName(folder_, cfg_.nameTemplate, tm, profile_.extension);
    }
    {
        // Keep the unique suffix (" (2)") in the base name so that _partNNN files stay related.
        const std::string first = uniqueOutputPath(folder_, baseName_, profile_.extension);
        std::string leaf = baseName(first);
        const std::string dotExt = profile_.extension.empty() ? "" : "." + profile_.extension;
        if (!dotExt.empty() && leaf.size() > dotExt.size() &&
            leaf.compare(leaf.size() - dotExt.size(), dotExt.size(), dotExt) == 0)
            leaf.resize(leaf.size() - dotExt.size());
        baseName_ = leaf;
    }

    splitBytesEff_ = cfg_.splitBytes;
    if (profile_.extension == "wav" || profile_.extension == "avi") {
        constexpr std::uint64_t kCap = 3900ull * 1000 * 1000;  // 32-bit RIFF sizes
        splitBytesEff_ = splitBytesEff_ ? std::min(splitBytesEff_, kCap) : kCap;
    }

    // ---- reset runtime state ----
    {
        std::lock_guard lk(mu_);
        vq_.clear();
        firstVideo_ = false;
        firstVideoNs_ = 0;
        dirty_ = false;
        ringCap_ = hasAudio_ ? std::max<std::size_t>(1024, static_cast<std::size_t>(cfg_.audioRingSeconds * inRate_)) : 0;
        ring_.assign(ringCap_ * static_cast<std::size_t>(std::max(channels_, 0)), 0.0f);
        ringHead_ = ringCount_ = 0;
        pushedAbs_ = nextAbs_ = 0;
    }
    droppedVideo_ = droppedAudio_ = 0;
    stage_.clear();
    stageFrames_ = 0;
    audioWritten_ = slots_ = repeated_ = silence_ = 0;
    segAudioBase_ = segSlotBase_ = 0;
    bytesDone_ = 0;
    lastFrame_.reset();
    segment_ = 0;
    haveT0_ = false;
    zeros_.assign(kChunkFrames * static_cast<std::size_t>(std::max(channels_, 1)), 0.0f);
    stopReq_ = false;
    {
        std::lock_guard lk(statusMu_);
        status_ = Status{};
    }

    std::string e;
    if (!openSegment(e)) return failStart(e);
    lastDiskCheckNs_ = now();
    {
        std::lock_guard lk(statusMu_);
        status_.freeBytes = freeNow;
        status_.segment = 1;
    }
    state_ = kRecording;
    thread_ = std::thread([this] { run(); });
    log(core::Level::Info, "Gravacao iniciada: " + baseName(status().file) + " (" + profile_.id + ")");
    publish("recording.started", {{"path", status().file}, {"file", baseName(status().file)}, {"profile", profile_.id}});
    return true;
}

void Recorder::stop() {
    {
        std::lock_guard lk(mu_);
        if (writerThreadId_ == std::this_thread::get_id()) {
            stopReq_ = true;  // called from an EventBus handler running on the writer thread: cannot join
            return;
        }
    }
    std::lock_guard ctl(ctlMu_);
    if (!thread_.joinable()) return;
    int expected = kRecording;
    state_.compare_exchange_strong(expected, kStopping);
    stopReq_ = true;
    {
        std::lock_guard lk(mu_);
        dirty_ = true;
    }
    cv_.notify_all();
    thread_.join();
}

void Recorder::pushVideo(std::shared_ptr<const video::Frame> frame) {
    if (!frame || state_.load() != kRecording || !hasVideo_) return;
    const video::Frame& f = *frame;
    const bool okDims = f.width == cfg_.video.width && f.height == cfg_.video.height && f.stride >= f.width * 4 &&
                        f.data.size() >= static_cast<std::size_t>(f.stride) * static_cast<std::size_t>(f.height - 1) +
                                             static_cast<std::size_t>(f.width) * 4;
    if (!okDims) {
        ++droppedVideo_;
        return;
    }
    {
        std::lock_guard lk(mu_);
        if (!firstVideo_) {
            firstVideo_ = true;
            firstVideoNs_ = now();
        }
        if (vq_.size() >= static_cast<std::size_t>(cfg_.videoQueueFrames)) {
            vq_.pop_front();
            ++droppedVideo_;
        }
        vq_.push_back(std::move(frame));
        dirty_ = true;
    }
    cv_.notify_one();
}

void Recorder::pushAudio(const float* in, std::size_t frames) {
    if (!in || frames == 0 || state_.load() != kRecording || !hasAudio_) return;
    {
        std::lock_guard lk(mu_);
        if (hasVideo_ && !firstVideo_) return;  // pre-roll before the first video frame is discarded (A/V alignment)
        const std::size_t ch = static_cast<std::size_t>(channels_);
        std::size_t n = frames;
        const float* src = in;
        const std::size_t total = ringCount_ + n;
        if (total > ringCap_) {
            const std::size_t evict = total - ringCap_;
            droppedAudio_ += evict;
            const std::size_t fromRing = std::min(evict, ringCount_);
            ringHead_ = (ringHead_ + fromRing) % ringCap_;
            ringCount_ -= fromRing;
            const std::size_t skip = evict - fromRing;
            src += skip * ch;
            n -= skip;
        }
        std::size_t tail = (ringHead_ + ringCount_) % ringCap_;
        const std::size_t first = std::min(n, ringCap_ - tail);
        std::memcpy(&ring_[tail * ch], src, first * ch * sizeof(float));
        if (n > first) std::memcpy(&ring_[0], src + first * ch, (n - first) * ch * sizeof(float));
        ringCount_ += n;
        pushedAbs_ += frames;
        dirty_ = true;
    }
    cv_.notify_one();
}

std::int64_t Recorder::slotPtsNs(std::uint64_t slotInSegment) const {
    return ratioNs(slotInSegment, static_cast<std::uint64_t>(cfg_.video.fpsDen), static_cast<std::uint64_t>(cfg_.video.fpsNum));
}
std::int64_t Recorder::audioPtsNs(std::uint64_t frameInSegment) const {
    return ratioNs(frameInSegment, 1, static_cast<std::uint64_t>(rate_));
}
std::uint64_t Recorder::slotEndAudioFrame(std::uint64_t slot) const {
    const std::uint64_t num = static_cast<std::uint64_t>(cfg_.video.fpsNum), den = static_cast<std::uint64_t>(cfg_.video.fpsDen);
    return ((slot + 1) * static_cast<std::uint64_t>(rate_) * den + num / 2) / num;
}

void Recorder::fail(const std::string& message) {
    std::string ignore;
    closeWriter(ignore);
    {
        std::lock_guard lk(statusMu_);
        status_.error = message;
    }
    log(core::Level::Error, "Gravacao interrompida: " + message);
    // Publish before flipping the publicly visible state: a caller that polls status() and
    // observes State::Error as soon as it changes must already be able to see this event (and the
    // log line above) — otherwise a fast poller can race ahead of the event it is looking for.
    publish("recording.error", {{"message", message}, {"file", baseName(status().file)}});
    state_ = kError;
}

bool Recorder::checkDisk(bool force) {
    const std::int64_t t = now();
    if (!force && t - lastDiskCheckNs_ < 1000000000ll) return true;
    lastDiskCheckNs_ = t;
    const std::uint64_t freeB = cfg_.freeSpaceBytes(folder_);
    {
        std::lock_guard lk(statusMu_);
        status_.freeBytes = freeB;
    }
    if (freeB < cfg_.minFreeBytes) {
        fail("Disco cheio: restam " + mb(freeB) + " livres (mínimo " + mb(cfg_.minFreeBytes) + "); arquivo finalizado");
        return false;
    }
    return true;
}

bool Recorder::writeStageAudio(std::uint64_t upTo, bool) {
    const std::size_t ch = static_cast<std::size_t>(channels_);
    while (audioWritten_ < upTo && !stage_.empty()) {
        StageChunk& c = stage_.front();
        const std::uint64_t n = std::min<std::uint64_t>({c.frames - c.used, upTo - audioWritten_, kChunkFrames});
        const float* p = c.silence ? zeros_.data() : c.data.data() + c.used * ch;
        std::string err;
        bool ok = false;
        try {
            ok = writer_->writeAudio(p, static_cast<std::size_t>(n), audioPtsNs(audioWritten_ - segAudioBase_), err);
        } catch (...) {
            err = "excecao no escritor";
        }
        if (!ok) {
            fail(err.empty() ? "falha ao gravar o audio" : err);
            return false;
        }
        c.used += n;
        audioWritten_ += n;
        stageFrames_ -= n;
        if (c.used == c.frames) stage_.pop_front();
    }
    return true;
}

bool Recorder::rollSegment() {
    std::string e;
    closeWriter(e);
    if (!e.empty()) {
        fail(e);
        return false;
    }
    if (!openSegment(e)) {
        fail(e);
        return false;
    }
    segAudioBase_ = audioWritten_;
    segSlotBase_ = slots_;
    log(core::Level::Info, "Novo segmento: " + baseName(status().file));
    publish("recording.segment", {{"path", status().file}, {"file", baseName(status().file)}, {"segment", std::to_string(segment_)}});
    return true;
}

bool Recorder::emitSlot(std::uint64_t slot, std::uint64_t audioEnd, bool final) {
    // segment boundary (checked lazily so that no empty trailing segment is ever created)
    const std::uint64_t segSlots = slot - segSlotBase_;
    const bool timeFull = cfg_.splitSeconds > 0 && segSlots * static_cast<std::uint64_t>(cfg_.video.fpsDen) >=
                                                       static_cast<std::uint64_t>(cfg_.splitSeconds) *
                                                           static_cast<std::uint64_t>(cfg_.video.fpsNum);
    const bool bytesFull = splitBytesEff_ > 0 && writer_ && writer_->stats().bytesWritten >= splitBytesEff_;
    if ((timeFull || bytesFull) && segSlots > 0 && !rollSegment()) return false;

    if (hasAudio_ && !writeStageAudio(audioEnd, final)) return false;

    std::shared_ptr<const video::Frame> f;
    bool repeat = false;
    {
        std::lock_guard lk(mu_);
        if (!vq_.empty()) {
            f = std::move(vq_.front());
            vq_.pop_front();
        }
    }
    if (f)
        lastFrame_ = f;
    else {
        f = lastFrame_;
        repeat = true;
    }
    if (!f) {  // cannot happen (first frame is queued before firstVideo_ is set); stay safe
        ++slots_;
        return true;
    }
    std::string err;
    bool ok = false;
    try {
        ok = writer_->writeVideo(f->data.data(), f->stride, slotPtsNs(slot - segSlotBase_), err);
    } catch (...) {
        err = "excecao no escritor";
    }
    if (!ok) {
        fail(err.empty() ? "falha ao gravar o video" : err);
        return false;
    }
    ++slots_;
    if (repeat) ++repeated_;
    return true;
}

bool Recorder::step(bool final) {
    bool haveFirst = !hasVideo_;
    std::int64_t t0 = 0;
    // ---- drain the audio ring into the stage (converting the rate when needed) ----
    std::vector<float> taken;
    std::uint64_t gap = 0;
    {
        std::lock_guard lk(mu_);
        if (hasVideo_ && firstVideo_) {
            haveFirst = true;
            t0 = firstVideoNs_;
        }
        if (hasAudio_) {
            const std::uint64_t ringBegin = pushedAbs_ - ringCount_;
            if (nextAbs_ < ringBegin) {
                gap = ringBegin - nextAbs_;
                nextAbs_ = ringBegin;
            }
            const std::uint64_t limit = final ? ~0ull : ringCap_;
            const std::uint64_t room = limit > stageFrames_ ? limit - stageFrames_ : 0;
            const std::size_t take = static_cast<std::size_t>(std::min<std::uint64_t>(ringCount_, room));
            if (take > 0) {
                const std::size_t ch = static_cast<std::size_t>(channels_);
                taken.resize(take * ch);
                const std::size_t first = std::min(take, ringCap_ - ringHead_);
                std::memcpy(taken.data(), &ring_[ringHead_ * ch], first * ch * sizeof(float));
                if (take > first) std::memcpy(taken.data() + first * ch, &ring_[0], (take - first) * ch * sizeof(float));
                ringHead_ = (ringHead_ + take) % ringCap_;
                ringCount_ -= take;
                nextAbs_ += take;
            }
        }
    }
    if (gap > 0) {  // evicted audio: keep the timeline with silence
        const std::uint64_t g = inRate_ == rate_ ? gap : gap * static_cast<std::uint64_t>(rate_) / static_cast<std::uint64_t>(inRate_);
        if (g > 0) {
            StageChunk c;
            c.silence = true;
            c.frames = g;
            stage_.push_back(std::move(c));
            stageFrames_ += g;
            silence_ += g;
        }
    }
    if (!taken.empty()) {
        const std::size_t ch = static_cast<std::size_t>(channels_);
        const std::size_t inFrames = taken.size() / ch;
        StageChunk c;
        if (!resampler_) {
            c.frames = inFrames;
            c.data = std::move(taken);
        } else {
            std::size_t produced = 0;
            for (std::size_t off = 0; off < inFrames; off += 4096) {
                const std::size_t n = std::min<std::size_t>(4096, inFrames - off);
                const std::size_t cap = resampler_->maxOutputFrames(n);
                resampleOut_.resize(cap * ch);
                const std::size_t o = resampler_->process(taken.data() + off * ch, n, resampleOut_.data(), cap);
                c.data.insert(c.data.end(), resampleOut_.begin(), resampleOut_.begin() + static_cast<std::ptrdiff_t>(o * ch));
                produced += o;
            }
            c.frames = produced;
        }
        if (c.frames > 0) {
            stageFrames_ += c.frames;
            stage_.push_back(std::move(c));
        }
    }

    if (hasVideo_) {
        if (haveFirst) {
            if (!haveT0_) {
                t0Ns_ = t0;
                haveT0_ = true;
            }
            const std::int64_t tolNs = static_cast<std::int64_t>(cfg_.audioStallToleranceMs) * 1000000ll;
            for (;;) {
                const std::uint64_t k = slots_;
                std::uint64_t E = 0;
                bool due = false;
                if (hasAudio_) {
                    E = slotEndAudioFrame(k);
                    const std::uint64_t avail = audioWritten_ + stageFrames_;
                    if (avail >= E) {
                        due = true;
                    } else if (!final) {
                        // audio stalled: if this slot ended on the wall clock more than the tolerance ago,
                        // fill with silence so that video does not freeze
                        const std::int64_t slotEndWall = ratioNs(E, 1, static_cast<std::uint64_t>(rate_));
                        if (now() - t0Ns_ > slotEndWall + tolNs) {
                            StageChunk c;
                            c.silence = true;
                            c.frames = E - avail;
                            stageFrames_ += c.frames;
                            silence_ += c.frames;
                            stage_.push_back(std::move(c));
                            due = true;
                        }
                    }
                } else {
                    due = now() - t0Ns_ >= slotPtsNs(k + 1);
                }
                if (!due) break;
                if (!emitSlot(k, E, final)) return false;
            }
            if (final) {
                for (;;) {
                    bool queued;
                    {
                        std::lock_guard lk(mu_);
                        queued = !vq_.empty();
                    }
                    const std::uint64_t k = slots_;
                    if (hasAudio_) {
                        const std::uint64_t avail = audioWritten_ + stageFrames_;
                        const std::uint64_t startFrame =
                            k * static_cast<std::uint64_t>(rate_) * static_cast<std::uint64_t>(cfg_.video.fpsDen) /
                            static_cast<std::uint64_t>(cfg_.video.fpsNum);
                        if (!(queued && startFrame < avail)) break;
                        if (!emitSlot(k, avail, true)) return false;
                    } else {
                        if (!queued) break;
                        if (!emitSlot(k, 0, true)) return false;
                    }
                }
            }
        }
        if (final && hasAudio_ && !writeStageAudio(audioWritten_ + stageFrames_, true)) return false;
    } else {
        // audio only: write what we have, splitting at exact sample boundaries
        const std::uint64_t splitFrames = cfg_.splitSeconds > 0 ? static_cast<std::uint64_t>(cfg_.splitSeconds) * static_cast<std::uint64_t>(rate_) : 0;
        while (stageFrames_ > 0) {
            const std::uint64_t segFrames = audioWritten_ - segAudioBase_;
            const bool full = (splitFrames > 0 && segFrames >= splitFrames) ||
                              (splitBytesEff_ > 0 && writer_ && writer_->stats().bytesWritten >= splitBytesEff_);
            if (full && segFrames > 0 && !rollSegment()) return false;
            std::uint64_t upTo = audioWritten_ + std::min<std::uint64_t>(stageFrames_, kChunkFrames);
            if (splitFrames > 0) upTo = std::min(upTo, segAudioBase_ + splitFrames);
            if (upTo <= audioWritten_) upTo = audioWritten_ + 1;
            if (!writeStageAudio(upTo, final)) return false;
        }
    }
    return true;
}

void Recorder::run() {
    {
        std::lock_guard lk(mu_);
        writerThreadId_ = std::this_thread::get_id();
    }
    try {
        for (;;) {
            {
                std::unique_lock lk(mu_);
                cv_.wait_for(lk, std::chrono::milliseconds(5), [&] { return dirty_ || stopReq_.load(); });
                dirty_ = false;
            }
            const bool final = stopReq_.load();
            if (state_.load() == kError) break;
            if (!step(final)) break;
            if (!final && !checkDisk(false)) break;
            updateStatus();
            if (final) break;
        }
    } catch (...) {
        fail("erro interno do gravador");
    }
    if (state_.load() != kError) {
        std::string e;
        closeWriter(e);
        if (!e.empty())
            fail(e);
        else {
            updateStatus();
            const Status s = status();
            log(core::Level::Info, "Gravacao finalizada: " + baseName(s.file));
            // Published before state_ flips to Idle, same reasoning as fail(): a caller polling
            // status() must never observe the new state before this event.
            publish("recording.stopped", {{"path", s.file},
                                          {"file", baseName(s.file)},
                                          {"seconds", std::to_string(s.seconds)},
                                          {"bytes", std::to_string(s.bytes)},
                                          {"segments", std::to_string(s.segment)}});
            state_ = kIdle;
        }
    }
    updateStatus();
    std::lock_guard lk(mu_);
    writerThreadId_ = std::thread::id();
}

}  // namespace hb::rec
