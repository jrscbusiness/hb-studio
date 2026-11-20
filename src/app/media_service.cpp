#include "app/media_service.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "audio/plugin_effect.hpp"
#include "core/json.hpp"
#include "recording/media_writer.hpp"
#include "plugins/native_source.hpp"
#include "vst3/vst3.hpp"
#ifdef _WIN32
#include "recording/windows/mf_writers.hpp"
#endif

namespace hb::app {

namespace {
namespace fs = std::filesystem;
namespace js = core::json;

std::vector<std::string> splitMsg(const std::string& msg, std::size_t maxParts) {
    std::vector<std::string> a;
    std::size_t s0 = 0;
    for (std::size_t i = 0; i <= msg.size(); ++i)
        if (i == msg.size() || (msg[i] == '|' && a.size() + 1 < maxParts)) {
            a.push_back(msg.substr(s0, i - s0));
            s0 = i + 1;
        }
    return a;
}
bool toInt(const std::string& s, int& out) {
    auto r = std::from_chars(s.data(), s.data() + s.size(), out);
    return r.ec == std::errc() && r.ptr == s.data() + s.size();
}
bool toDouble(const std::string& s, double& out) {
    if (s.empty()) return false;
    try {
        std::size_t n = 0;
        out = std::stod(s, &n);
        return n == s.size() && std::isfinite(out);
    } catch (...) { return false; }
}
std::string q(const std::string& s) { return "\"" + js::escape(s) + "\""; }
std::string readFile(const std::string& p) {
    std::ifstream f(fs::u8path(p), std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
bool writeAtomic(const std::string& p, const std::string& text) {
    const std::string tmp = p + ".tmp";
    {
        std::ofstream f(fs::u8path(tmp), std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << text;
        if (!f) return false;
    }
    std::error_code ec;
    fs::rename(fs::u8path(tmp), fs::u8path(p), ec);
    return !ec;
}
int clampi(int v, int lo, int hi) { return std::min(std::max(v, lo), hi); }
}  // namespace

MediaService::MediaService(video::ProgramOutput& program, core::EventBus* bus, core::Logger* log, std::string dataDir,
                           std::string defaultFolder, audio_io::EngineOptions opts)
    : program_(program), bus_(bus), log_(log), dataDir_(std::move(dataDir)), defaultFolder_(std::move(defaultFolder)),
      audio_((audio::registerBuiltinEffects(), rec::registerPortableWriters(),
#ifdef _WIN32
              rec::registerWindowsWriters(),
#endif
              std::move(opts))) {
    rs_.folder = defaultFolder_;
}

MediaService::~MediaService() { stop(); }

void MediaService::start() {
    if (started_) return;
    started_ = true;
    loadSettings();
    if (!dataDir_.empty()) {
        std::error_code ec;
        fs::create_directories(fs::u8path(dataDir_), ec);
        if (audio_.loadSettings(dataDir_ + "/audio.json") && log_) log_->info("audio", "configuração de áudio carregada");
        audioVersionSaved_ = audio_.version();
    }
    audio_.start();
}

void MediaService::stop() {
    if (!started_) return;
    started_ = false;
    stopRecording();
    saveSettings();
    if (!dataDir_.empty()) audio_.saveSettings(dataDir_ + "/audio.json");
    audio_.stop();
}

void MediaService::loadSettings() {
    if (dataDir_.empty()) return;
    js::Value v;
    const std::string text = readFile(dataDir_ + "/recording.json");
    if (text.empty() || !js::parse(text, v) || !v.isObj()) return;
    auto str = [&](const char* k, std::string& dst, std::size_t maxLen) {
        if (auto* x = v.get(k); x && x->isStr() && x->s.size() <= maxLen) dst = x->s;
    };
    auto num = [&](const char* k, int& dst, int lo, int hi) {
        if (auto* x = v.get(k); x && x->isNum() && std::isfinite(x->n)) dst = clampi(static_cast<int>(x->n), lo, hi);
    };
    std::lock_guard lock(mu_);
    str("profile", rs_.profileId, 64);
    str("folder", rs_.folder, 1024);
    str("template", rs_.nameTemplate, 128);
    num("videoKbps", rs_.videoBitrateKbps, 500, 100000);
    num("videoQuality", rs_.videoQuality, 1, 100);
    num("audioKbps", rs_.audioBitrateKbps, 32, 320);
    num("audioRate", rs_.audioSampleRate, 44100, 96000);
    num("audioBits", rs_.audioBitsPerSample, 16, 32);
    num("keyframeSec", rs_.keyframeIntervalSec, 1, 10);
    num("splitMin", rs_.splitMinutes, 0, 1440);
    num("splitMB", rs_.splitMegabytes, 0, 100000);
    num("minFreeMB", rs_.minFreeMegabytes, 50, 100000);
    if (rs_.folder.empty()) rs_.folder = defaultFolder_;
    if (rs_.audioSampleRate != 44100 && rs_.audioSampleRate != 48000 && rs_.audioSampleRate != 96000) rs_.audioSampleRate = 48000;
}

void MediaService::saveSettings() {
    if (dataDir_.empty()) return;
    RecordingSettings r;
    { std::lock_guard lock(mu_); r = rs_; }
    std::ostringstream o;
    o << "{\"profile\":" << q(r.profileId) << ",\"folder\":" << q(r.folder) << ",\"template\":" << q(r.nameTemplate)
      << ",\"videoKbps\":" << r.videoBitrateKbps << ",\"videoQuality\":" << r.videoQuality
      << ",\"audioKbps\":" << r.audioBitrateKbps << ",\"audioRate\":" << r.audioSampleRate
      << ",\"audioBits\":" << r.audioBitsPerSample << ",\"keyframeSec\":" << r.keyframeIntervalSec
      << ",\"splitMin\":" << r.splitMinutes << ",\"splitMB\":" << r.splitMegabytes << ",\"minFreeMB\":" << r.minFreeMegabytes << "}";
    writeAtomic(dataDir_ + "/recording.json", o.str());
}

RecordingSettings MediaService::settings() const { std::lock_guard lock(mu_); return rs_; }
bool MediaService::recording() const { std::lock_guard lock(mu_); return recorder_ != nullptr; }

bool MediaService::setFolder(const std::string& folder) {
    if (folder.empty() || folder.size() > 1024) return false;
    { std::lock_guard lock(mu_); rs_.folder = folder; ++recSettingsVer_; }
    saveSettings();
    return true;
}

// ---- recording ---------------------------------------------------------------------------------------------

bool MediaService::startRecording(std::string& error) {
    RecordingSettings r;
    {
        std::lock_guard lock(mu_);
        if (recorder_) { error = "já está gravando"; return false; }
        r = rs_;
    }
    rec::RecordingProfile prof;
    rec::WriterFactory fac;
    if (!rec::WriterRegistry::instance().find(r.profileId, prof, fac)) {
        error = "formato de gravação indisponível: " + r.profileId;
        return false;
    }
    rec::RecorderConfig cfg;
    cfg.profileId = r.profileId;
    cfg.folderUtf8 = r.folder.empty() ? defaultFolder_ : r.folder;
    cfg.nameTemplate = r.nameTemplate.empty() ? "HBStudio_{date}_{time}" : r.nameTemplate;
    cfg.profile = prof;
    cfg.profile.videoBitrateKbps = r.videoBitrateKbps;
    cfg.profile.videoQuality = r.videoQuality;
    cfg.profile.audioBitrateKbps = r.audioBitrateKbps;
    cfg.profile.audioSampleRate = r.audioSampleRate;
    cfg.profile.audioBitsPerSample = r.audioBitsPerSample;
    cfg.profile.keyframeIntervalSec = r.keyframeIntervalSec;
    const auto& pf = program_.format();
    cfg.video.width = pf.width;
    cfg.video.height = pf.height;
    cfg.video.fpsNum = pf.fps.num;
    cfg.video.fpsDen = pf.fps.den;
    cfg.audio = {48000, 2};
    cfg.splitSeconds = static_cast<std::int64_t>(r.splitMinutes) * 60;
    cfg.splitBytes = static_cast<std::uint64_t>(r.splitMegabytes) << 20;
    cfg.minFreeBytes = static_cast<std::uint64_t>(r.minFreeMegabytes) << 20;

    auto recorder = std::make_shared<rec::Recorder>(cfg, bus_, log_);
    if (!recorder->start(error)) return false;
    const bool hasV = prof.hasVideo, hasA = prof.hasAudio;
    std::weak_ptr<rec::Recorder> weak = recorder;
    {
        std::lock_guard lock(mu_);
        recorder_ = recorder;
        haveV_ = hasV;
        haveA_ = hasA;
        lastRecError_.clear();
    }
    if (hasV)
        vConsumer_ = program_.addConsumer([weak](const std::shared_ptr<const video::Frame>& f) {
            if (auto rr = weak.lock()) rr->pushVideo(f);
        });
    if (hasA)
        aConsumer_ = audio_.addProgramConsumer([weak](const float* s, std::size_t n) {
            if (auto rr = weak.lock()) rr->pushAudio(s, n);
        });
    return true;
}

void MediaService::stopRecording() {
    std::shared_ptr<rec::Recorder> r;
    bool v, a;
    {
        std::lock_guard lock(mu_);
        r = recorder_;
        v = haveV_; a = haveA_;
    }
    if (!r) return;
    if (v && vConsumer_) program_.removeConsumer(vConsumer_);     // synchronous: no more frames after this
    if (a && aConsumer_) audio_.removeProgramConsumer(aConsumer_);
    vConsumer_ = 0; aConsumer_ = 0;
    r->stop();                                                    // drains and finalises the file
    {
        std::lock_guard lock(mu_);
        const auto st = r->status();
        if (st.state == rec::Recorder::Status::State::Error) lastRecError_ = st.error;
        lastFile_ = st.file;
        recorder_.reset();
    }
}

std::string MediaService::recStatusJson() {
    std::shared_ptr<rec::Recorder> r;
    std::string err, lastFile;
    {
        std::lock_guard lock(mu_);
        r = recorder_;
        err = lastRecError_;
        lastFile = lastFile_;
    }
    std::ostringstream o;
    o << "{\"t\":\"rec\",";
    if (!r) {
        o << "\"state\":\"idle\",\"error\":" << q(err) << ",\"last\":" << q(lastFile) << "}";
        return o.str();
    }
    const auto s = r->status();
    const char* st = s.state == rec::Recorder::Status::State::Recording ? "recording"
                     : s.state == rec::Recorder::Status::State::Stopping ? "stopping" : s.state == rec::Recorder::Status::State::Error ? "error" : "idle";
    o << "\"state\":\"" << st << "\",\"file\":" << q(s.file) << ",\"seconds\":" << s.seconds << ",\"bytes\":" << s.bytes
      << ",\"freeBytes\":" << s.freeBytes << ",\"segment\":" << s.segment << ",\"droppedVideo\":" << s.droppedVideo
      << ",\"repeatedVideo\":" << s.repeatedVideo << ",\"droppedAudio\":" << s.droppedAudio << ",\"error\":" << q(s.error) << "}";
    return o.str();
}

std::string MediaService::recProfilesJson() {
    RecordingSettings r;
    unsigned ver;
    { std::lock_guard lock(mu_); r = rs_; ver = recSettingsVer_; }
    std::ostringstream o;
    o << "{\"t\":\"recprofiles\",\"ver\":" << ver << ",\"profiles\":[";
    bool first = true;
    for (const auto& p : rec::WriterRegistry::instance().profiles()) {
        if (!first) o << ",";
        first = false;
        o << "{\"id\":" << q(p.id) << ",\"label\":" << q(p.label) << ",\"desc\":" << q(p.description) << ",\"ext\":" << q(p.extension)
          << ",\"kind\":" << q(p.kind) << ",\"video\":" << (p.hasVideo ? "true" : "false") << ",\"audio\":" << (p.hasAudio ? "true" : "false") << "}";
    }
    const auto& pf = program_.format();
    o << "],\"cfg\":{\"profile\":" << q(r.profileId) << ",\"folder\":" << q(r.folder) << ",\"template\":" << q(r.nameTemplate)
      << ",\"videoKbps\":" << r.videoBitrateKbps << ",\"videoQuality\":" << r.videoQuality << ",\"audioKbps\":" << r.audioBitrateKbps
      << ",\"audioRate\":" << r.audioSampleRate << ",\"audioBits\":" << r.audioBitsPerSample << ",\"keyframeSec\":" << r.keyframeIntervalSec
      << ",\"splitMin\":" << r.splitMinutes << ",\"splitMB\":" << r.splitMegabytes << ",\"minFreeMB\":" << r.minFreeMegabytes
      << "},\"video\":{\"w\":" << pf.width << ",\"h\":" << pf.height << ",\"fps\":" << (pf.fps.den ? static_cast<double>(pf.fps.num) / pf.fps.den : 30.0) << "}}";
    return o.str();
}

// ---- commands ----------------------------------------------------------------------------------------------

std::string MediaService::handle(const std::string& msg) {
    try {
        const std::string c0 = msg.substr(0, msg.find('|'));
        auto a = splitMsg(msg, 8);
        if (a.size() < 2) return "";
        const std::string& sub = a[1];
        auto fail = [&](bool ok) { return ok ? std::string() : audio_.lastError(); };
        if (c0 == "aud") {
            int i = -1;
            if (a.size() > 2) toInt(a[2], i);
            if (sub == "strip" && a.size() >= 5) {
                double d;
                if (a[3] == "name") return fail(audio_.setStripParam(i, "name", a[4]));
                if (!toDouble(a[4], d)) return "valor inválido";
                return fail(audio_.setStripParam(i, a[3], d));
            }
            if (sub == "quality" && a.size() >= 4) return fail(audio_.setStripQuality(i, a[3]));
            if (sub == "input" && a.size() >= 4) return fail(audio_.setStripInput(i, a[3], a.size() > 4 ? a[4] : ""));
            if (sub == "fxadd" && a.size() >= 4) return fail(audio_.addEffect(i, a[3]));
            if (sub == "fxdel" && a.size() >= 4) { int k; return toInt(a[3], k) ? fail(audio_.removeEffect(i, k)) : "índice inválido"; }
            if (sub == "fxmove" && a.size() >= 5) { int f, t; return toInt(a[3], f) && toInt(a[4], t) ? fail(audio_.moveEffect(i, f, t)) : "índice inválido"; }
            if (sub == "fxbyp" && a.size() >= 5) { int k; return toInt(a[3], k) ? fail(audio_.setEffectBypass(i, k, a[4] == "1")) : "índice inválido"; }
            if (sub == "fxparam" && a.size() >= 6) {
                int k; double d;
                if (!toInt(a[3], k) || !toDouble(a[5], d)) return "valor inválido";
                return fail(audio_.setEffectParam(i, k, a[4], d));
            }
            if (sub == "master" && a.size() >= 4) { double d; return toDouble(a[3], d) ? fail(audio_.setMasterParam(a[2], d)) : "valor inválido"; }
            if (sub == "monitor" && a.size() >= 4) {
                if (a[2] == "deviceId") return fail(audio_.setMonitor("deviceId", a[3]));
                double d;
                return toDouble(a[3], d) ? fail(audio_.setMonitor(a[2], d)) : "valor inválido";
            }
            if (sub == "refresh") { audio_.refreshDevices(); return ""; }
            if (sub == "reset") { audio_.resetMeters(); return ""; }
            return "";
        }
        if (c0 == "rec") {
            if (sub == "start") {
                std::string err;
                if (!startRecording(err)) {
                    std::lock_guard lock(mu_);
                    lastRecError_ = err;
                    return "Não foi possível gravar: " + err;
                }
                return "Gravação iniciada.";
            }
            if (sub == "stop") {
                stopRecording();
                return "Gravação finalizada.";
            }
            if (sub == "cfg" && a.size() >= 4) {      // rec|cfg|key|value
                if (recording()) return "Pare a gravação para mudar o formato.";
                const std::string& k = a[2];
                const std::string& val = a[3];
                int n = 0;
                std::lock_guard lock(mu_);
                if (k == "profile") {
                    rec::RecordingProfile p; rec::WriterFactory f;
                    if (!rec::WriterRegistry::instance().find(val, p, f)) return "formato indisponível";
                    rs_.profileId = val;
                    rs_.audioBitrateKbps = p.audioBitrateKbps; rs_.videoBitrateKbps = p.videoBitrateKbps;
                    rs_.audioSampleRate = p.audioSampleRate; rs_.audioBitsPerSample = p.audioBitsPerSample;
                    rs_.videoQuality = p.videoQuality;
                } else if (k == "template") {
                    if (val.empty() || val.size() > 128) return "modelo inválido";
                    rs_.nameTemplate = val;
                } else if (k == "folder") {
                    if (val.empty() || val.size() > 1024) return "pasta inválida";
                    rs_.folder = val;
                } else if (toInt(val, n)) {
                    if (k == "videoKbps") rs_.videoBitrateKbps = clampi(n, 500, 100000);
                    else if (k == "videoQuality") rs_.videoQuality = clampi(n, 1, 100);
                    else if (k == "audioKbps") rs_.audioBitrateKbps = clampi(n, 32, 320);
                    else if (k == "audioRate") rs_.audioSampleRate = (n == 44100 || n == 96000) ? n : 48000;
                    else if (k == "audioBits") rs_.audioBitsPerSample = (n == 16 || n == 32) ? n : 24;
                    else if (k == "keyframeSec") rs_.keyframeIntervalSec = clampi(n, 1, 10);
                    else if (k == "splitMin") rs_.splitMinutes = clampi(n, 0, 1440);
                    else if (k == "splitMB") rs_.splitMegabytes = clampi(n, 0, 100000);
                    else if (k == "minFreeMB") rs_.minFreeMegabytes = clampi(n, 50, 100000);
                    else return "";
                } else return "valor inválido";
                ++recSettingsVer_;
                saveSettingsLocked();
                return "";
            }
        }
    } catch (...) {
        return "erro interno ao processar o comando";
    }
    return "";
}

void MediaService::saveSettingsLocked() {
    // mu_ is held by the caller: copy and write without re-locking.
    if (dataDir_.empty()) return;
    const RecordingSettings& r = rs_;
    std::ostringstream o;
    o << "{\"profile\":" << q(r.profileId) << ",\"folder\":" << q(r.folder) << ",\"template\":" << q(r.nameTemplate)
      << ",\"videoKbps\":" << r.videoBitrateKbps << ",\"videoQuality\":" << r.videoQuality
      << ",\"audioKbps\":" << r.audioBitrateKbps << ",\"audioRate\":" << r.audioSampleRate
      << ",\"audioBits\":" << r.audioBitsPerSample << ",\"keyframeSec\":" << r.keyframeIntervalSec
      << ",\"splitMin\":" << r.splitMinutes << ",\"splitMB\":" << r.splitMegabytes << ",\"minFreeMB\":" << r.minFreeMegabytes << "}";
    writeAtomic(dataDir_ + "/recording.json", o.str());
}

// ---- periodic ----------------------------------------------------------------------------------------------

void MediaService::tick(std::uint64_t n, const Post& post) {
    const unsigned v = audio_.version();
    if (v != audioVersionSent_) {
        audioVersionSent_ = v;
        post("audio", audio_.snapshotJson(true));
    }
    post("audiolv", audio_.snapshotJson(false));
    unsigned rv;
    { std::lock_guard lock(mu_); rv = recSettingsVer_; }
    if (rv != recSettingsSent_) {
        recSettingsSent_ = rv;
        post("recprofiles", recProfilesJson());
    }
    if (n % 4 == 0) {
        // The recorder can stop by itself (disk full, writer failure): reflect it.
        std::shared_ptr<rec::Recorder> r;
        { std::lock_guard lock(mu_); r = recorder_; }
        if (r && r->status().state == rec::Recorder::Status::State::Error) stopRecording();
        post("rec", recStatusJson());
    }
    if (n - lastSaveCheck_ >= 75) {     // ~5 s
        lastSaveCheck_ = n;
        if (v != audioVersionSaved_ && !dataDir_.empty()) {
            audioVersionSaved_ = v;
            audio_.saveSettings(dataDir_ + "/audio.json");
        }
    }
}

// ---- effect plugins ----------------------------------------------------------------------------------------

void MediaService::syncPluginEffects(plugins::PluginService& svc) {
    auto& reg = audio::EffectRegistry::instance();
    for (const auto& s : pluginFxSources_) reg.removeBySource(s);
    pluginFxSources_.clear();
    for (const auto& id : svc.manager().enabledNativeIds()) {
        auto np = svc.runtime().loaded(id);
        if (!np || !np->hasEffects()) continue;
        auto lib = std::make_shared<audio::PluginEffectLibrary>();
        lib->api = np->effectsApi();
        lib->keepAlive = std::static_pointer_cast<void>(np->library());
        std::vector<audio::PluginEffectDesc> descs;
        std::string err;
        if (!audio::describePluginEffects(*lib, descs, err)) {
            if (log_) log_->warning("plugins", "efeitos de " + id + " recusados: " + err);
            continue;
        }
        for (std::size_t k = 0; k < descs.size(); ++k) {
            const auto d = descs[k];
            const int index = static_cast<int>(k);
            audio::EffectRegistry::Info info{d.typeId, d.name, "Plugin", id};
            reg.add(info, [lib, index, d] { return audio::createPluginEffect(lib, index, d); });
        }
        pluginFxSources_.insert(id);
    }
    audioVersionSent_ = ~0u;   // refresh the "add effect" menu in the UI
}

void MediaService::syncVst3Effects(const std::vector<std::string>& extraFolders) {
    auto& reg = audio::EffectRegistry::instance();
    for (const auto& s : vst3FxSources_) reg.removeBySource(s);
    vst3FxSources_.clear();
    for (const auto& plugin : vst3::scanPlugins(extraFolders)) {
        const std::string typeId = vst3::typeIdFor(plugin);
        if (reg.contains(typeId)) continue;  // same classId found again (rescan): keep the first
        audio::EffectRegistry::Info info{typeId, plugin.name, "VST3", "vst3:" + plugin.path};
        reg.add(info, [plugin] {
            std::string err;  // createEffect() failures just mean addEffect()/the engine's
                               // "missing" handling applies, same as a vanished native plugin
            return vst3::createEffect(plugin, err);
        });
        vst3FxSources_.insert(info.source);
    }
    audioVersionSent_ = ~0u;
}

}  // namespace hb::app
