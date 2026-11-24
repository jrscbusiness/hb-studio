#include "multitrack/iso_session.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>

#include "multitrack/edl_export.hpp"
#include "multitrack/resource_estimator.hpp"
#include "recording/file_io.hpp"
#include "recording/file_naming.hpp"

namespace hb::multitrack {

namespace fs = std::filesystem;

namespace {

std::string codecOfAudio(const std::string& id) {
    if (id == "wav") return "pcm_s24le";
    if (id == "wav-16") return "pcm_s16le";
    if (id == "wav-f32") return "pcm_f32le";
    if (id.rfind("flac", 0) == 0) return "flac";
    if (id.rfind("m4a", 0) == 0) return "aac";
    return id;
}

std::string idOf(const char* prefix, int index0) {
    char b[24];
    std::snprintf(b, sizeof b, "%s%02d", prefix, index0 + 1);
    return b;
}

bool writeTextAtomic(const std::string& pathUtf8, const std::string& text) {
    const fs::path p = rec::detail::pathFromUtf8(pathUtf8);
    fs::path tmp = p;
    tmp += ".tmp";
    std::error_code ec;
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(text.data(), static_cast<std::streamsize>(text.size()));
        f.flush();
        if (!f) return false;
    }   // closed before the rename (required on Windows)
    fs::rename(tmp, p, ec);
    if (ec) {
        fs::remove(p, ec);
        ec.clear();
        fs::rename(tmp, p, ec);
    }
    return !ec;
}

}  // namespace

IsoSession::IsoSession(IsoSessionConfig cfg) : cfg_(std::move(cfg)), clock_(cfg_.nowNs, cfg_.wallMs) {
    if (!cfg_.freeSpace)
        cfg_.freeSpace = [](const std::string& folder) -> std::uint64_t {
            std::error_code ec;
            const auto sp = fs::space(rec::detail::pathFromUtf8(folder), ec);
            return ec ? ~0ull : static_cast<std::uint64_t>(sp.available);
        };
    if (!cfg_.findProfile)
        cfg_.findProfile = [](const std::string& id, rec::RecordingProfile& p, rec::WriterFactory& f) {
            return rec::WriterRegistry::instance().find(id, p, f);
        };
}

IsoSession::~IsoSession() { stop(); }

bool IsoSession::start(std::string& error) {
    if (started_.exchange(true)) {
        error = "sessao ja iniciada";
        return false;
    }
    std::error_code ec;
    const fs::path root = rec::detail::pathFromUtf8(cfg_.sessionDir);
    fs::create_directories(root / "iso", ec);
    if (!ec) fs::create_directories(root / "audio", ec);
    if (ec) {
        error = "nao foi possivel criar a pasta da sessao";
        return false;
    }
    clock_.start();
    freeBytes_ = cfg_.freeSpace(cfg_.sessionDir);

    TrackServices svc{&clock_, cfg_.bus, cfg_.log};
    int started = 0, requested = 0;

    for (const IsoVideoSpec& v : cfg_.video) {
        ++requested;
        std::unique_ptr<IsoVideoTrack> tr;
        rec::RecordingProfile prof;
        rec::WriterFactory fac;
        bool found = false;
        for (const auto& id : cfg_.videoProfileIds)
            if (cfg_.findProfile(id, prof, fac)) {
                found = true;
                break;
            }
        const std::string id = idOf("CAM", v.slot);
        if (!found) {
            warnings_.push_back(id + ": nenhum formato de vídeo H.264 disponível; trilha não iniciada.");
            vtracks_.push_back(nullptr);
            continue;
        }
        TrackSpec s;
        s.kind = TrackKind::Video;
        s.id = id;
        s.name = v.name;
        s.device = v.device;
        s.sourceIndex = v.slot;
        s.folder = rec::detail::pathToUtf8(root / "iso");
        s.relDir = "iso";
        s.stem = id + "_" + rec::sanitizeFileName(v.name.empty() ? "camera" : v.name);
        s.codec = "h264";
        s.profile = prof;
        s.profile.hasAudio = false;
        s.profile.videoBitrateKbps = v.bitrateKbps > 0 ? v.bitrateKbps : autoVideoBitrateKbps(v.width, v.height, double(v.fpsNum) / std::max(1, v.fpsDen));
        s.factory = fac;
        s.video = rec::VideoFormat{v.width & ~1, v.height & ~1, v.fpsNum, v.fpsDen};
        s.rotationSeconds = cfg_.rotationSeconds;
        s.queueDepth = cfg_.videoQueueDepth;
        tr = std::make_unique<IsoVideoTrack>(std::move(s), svc);
        std::string e;
        if (tr->start(e)) ++started;
        else warnings_.push_back(id + ": não foi possível iniciar (" + e + ").");
        vtracks_.push_back(std::move(tr));
    }

    for (const IsoAudioSpec& a : cfg_.audio) {
        ++requested;
        const std::string id = idOf("CH", a.strip);
        rec::RecordingProfile prof;
        rec::WriterFactory fac;
        if (!cfg_.findProfile(a.profileId, prof, fac)) {
            warnings_.push_back(id + ": formato de áudio '" + a.profileId + "' indisponível; trilha não iniciada.");
            atracks_.push_back(nullptr);
            continue;
        }
        TrackSpec s;
        s.kind = TrackKind::Audio;
        s.id = id;
        s.name = a.name;
        s.sourceIndex = a.strip;
        s.folder = rec::detail::pathToUtf8(root / "audio");
        s.relDir = "audio";
        s.stem = id + "_" + rec::sanitizeFileName(a.name.empty() ? "canal" : a.name);
        s.codec = codecOfAudio(a.profileId);
        s.tap = a.postFader ? "post-fader" : "pre-fader";
        s.profile = prof;
        s.profile.audioSampleRate = a.sampleRate;
        s.factory = fac;
        s.audio = rec::AudioFormat{a.sampleRate, a.channels};
        s.rotationSeconds = cfg_.rotationSeconds;
        auto tr = std::make_unique<IsoAudioTrack>(std::move(s), svc);
        std::string e;
        if (tr->start(e)) ++started;
        else warnings_.push_back(id + ": não foi possível iniciar (" + e + ").");
        atracks_.push_back(std::move(tr));
    }

    if (requested > 0 && started == 0) {
        error = warnings_.empty() ? "nenhuma trilha ISO pôde ser iniciada" : warnings_.front();
        stop();
        return false;
    }

    if (cfg_.startSamplers) {
        for (std::size_t i = 0; i < cfg_.video.size(); ++i) {
            if (!vtracks_[i] || !vtracks_[i]->active()) continue;
            const IsoVideoSpec& v = cfg_.video[i];
            IsoVideoTrack* t = vtracks_[i].get();
            auto smp = std::make_unique<VideoSampler>(
                v.width & ~1, v.height & ~1, v.fpsNum, v.fpsDen, v.render,
                [t](std::shared_ptr<const video::Frame> f, Ns ns) { t->pushVideo(std::move(f), ns); }, &clock_);
            smp->start();
            samplers_.push_back(std::move(smp));
        }
    }
    writeManifest("recording");
    if (cfg_.autoSupervise) super_ = std::thread([this] { superviseLoop(); });
    return true;
}

void IsoSession::superviseLoop() {
    std::unique_lock lk(superMu_);
    while (!superStop_) {
        if (superCv_.wait_for(lk, std::chrono::seconds(1), [&] { return superStop_; })) break;
        lk.unlock();
        try {
            superviseOnce();
        } catch (...) {
        }
        lk.lock();
    }
}

void IsoSession::superviseOnce() {
    if (!started_ || stopped_) return;
    const std::uint64_t freeB = cfg_.freeSpace(cfg_.sessionDir);
    freeBytes_ = freeB;
    if (freeB < cfg_.minFreeBytes) {
        bool any = false;
        for (auto& t : vtracks_) if (t && t->active()) { t->stopForDisk("Pouco espaço em disco: gravação ISO interrompida"); any = true; }
        for (auto& t : atracks_) if (t && t->active()) { t->stopForDisk("Pouco espaço em disco: gravação ISO interrompida"); any = true; }
        if (any) {
            std::lock_guard lk(mu_);
            warnings_.push_back("Espaço em disco abaixo do mínimo: as trilhas ISO foram finalizadas; a gravação do Program continua.");
            if (cfg_.log) cfg_.log->log(core::Level::Warning, "multitrack", "disco baixo: trilhas ISO interrompidas");
        }
        writeManifest("recording");
        return;
    }
    const Ns now = clock_.nowNs();
    if (now - lastManifestNs_ >= 5 * kNsPerSec) writeManifest("recording");
}

void IsoSession::pushAudio(std::size_t i, const float* p, std::size_t frames, Ns startNs) {
    if (i < atracks_.size() && atracks_[i]) atracks_[i]->pushAudio(p, frames, startNs);
}

void IsoSession::pushAudioNow(std::size_t i, const float* p, std::size_t frames) {
    if (i >= atracks_.size() || !atracks_[i]) return;
    const int rate = atracks_[i]->spec().audio.sampleRate;
    atracks_[i]->pushAudio(p, frames, clock_.nowNs() - unitsToNs(frames, static_cast<std::uint64_t>(std::max(1, rate)), 1));
}

void IsoSession::pushVideo(std::size_t i, std::shared_ptr<const video::Frame> f, Ns ns) {
    if (i < vtracks_.size() && vtracks_[i]) vtracks_[i]->pushVideo(std::move(f), ns);
}

void IsoSession::setProgramTrack(const ManifestTrack& t) {
    std::lock_guard lk(mu_);
    program_ = t;
    haveProgram_ = true;
}

void IsoSession::stop() {
    if (!started_ || stopped_.exchange(true)) return;
    {
        std::lock_guard lk(superMu_);
        superStop_ = true;
    }
    superCv_.notify_all();
    if (super_.joinable()) super_.join();
    for (auto& s : samplers_) s->stop();
    endNs_ = clock_.started() ? clock_.nowNs() : 0;
    for (auto& t : vtracks_) if (t) t->stop();
    for (auto& t : atracks_) if (t) t->stop();
    writeManifest("finished");
    if (cfg_.writeEdl) {
        const SessionManifest m = manifest("finished");
        for (const EdlFile& e : buildSessionEdls(m, cfg_.tcFpsNum, cfg_.tcFpsDen)) {
            std::error_code ec;
            const fs::path p = rec::detail::pathFromUtf8(cfg_.sessionDir) / rec::detail::pathFromUtf8(e.relativePath);
            fs::create_directories(p.parent_path(), ec);
            writeTextAtomic(rec::detail::pathToUtf8(p), e.content);
        }
    }
}

SessionManifest IsoSession::manifest(const std::string& state) const {
    SessionManifest m;
    m.appVersion = cfg_.appVersion;
    m.sessionName = cfg_.sessionName;
    m.state = state;
    m.startWallUnixMs = clock_.wallStartMs();
    m.monotonicT0Ns = clock_.t0Ns();
    m.durationNs = stopped_ ? endNs_ : (clock_.started() ? clock_.nowNs() : 0);
    m.rotationSeconds = cfg_.rotationSeconds;
    m.syncNote =
        "Todas as trilhas usam o mesmo relógio monotônico da sessão (zero = início da sessão). Áudio: a amostra 0 do "
        "arquivo é o início da sessão (silêncio inicial inserido). Vídeo: o primeiro quadro do arquivo está em "
        "startOffsetNs. Programa: startOffsetNs = primeiro quadro do Program.";
    std::lock_guard lk(mu_);
    if (haveProgram_) m.tracks.push_back(program_);
    for (const auto& t : vtracks_) if (t) m.tracks.push_back(t->manifest());
    for (const auto& t : atracks_) if (t) m.tracks.push_back(t->manifest());
    m.warnings = warnings_;
    return m;
}

bool IsoSession::writeManifest(const std::string& state) {
    lastManifestNs_ = clock_.started() ? clock_.nowNs() : 0;
    const std::string json = manifestToJson(manifest(state));
    const fs::path p = rec::detail::pathFromUtf8(cfg_.sessionDir) / "session.json";
    return writeTextAtomic(rec::detail::pathToUtf8(p), json);
}

IsoSessionStatus IsoSession::status() const {
    IsoSessionStatus s;
    s.active = started_ && !stopped_;
    s.seconds = clock_.started() ? static_cast<double>(stopped_ ? endNs_ : clock_.nowNs()) / 1e9 : 0.0;
    s.freeBytes = freeBytes_.load();
    s.sessionDir = cfg_.sessionDir;
    for (const auto& t : vtracks_) if (t) s.tracks.push_back(t->status());
    for (const auto& t : atracks_) if (t) s.tracks.push_back(t->status());
    std::lock_guard lk(mu_);
    s.warnings = warnings_;
    return s;
}

}  // namespace hb::multitrack
