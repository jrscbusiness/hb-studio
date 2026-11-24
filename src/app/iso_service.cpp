#include "app/iso_service.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "core/json.hpp"
#include "multitrack/resource_estimator.hpp"
#include "recording/file_io.hpp"
#include "recording/file_naming.hpp"

namespace hb::app {

namespace {
namespace fs = std::filesystem;
namespace js = core::json;
using namespace multitrack;

std::string q(const std::string& s) { return "\"" + js::escape(s) + "\""; }
std::string dbl(double v) {
    if (!std::isfinite(v)) return "0";
    char b[40];
    std::snprintf(b, sizeof b, "%.2f", v);
    return b;
}
bool toInt(const std::string& s, int& out) {
    auto r = std::from_chars(s.data(), s.data() + s.size(), out);
    return r.ec == std::errc() && r.ptr == s.data() + s.size();
}
int clampi(int v, int lo, int hi) { return std::min(std::max(v, lo), hi); }

std::string readFile(const std::string& p) {
    std::ifstream f(rec::detail::pathFromUtf8(p), std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
bool writeAtomic(const std::string& p, const std::string& text) {
    const std::string tmp = p + ".tmp";
    {
        std::ofstream f(rec::detail::pathFromUtf8(tmp), std::ios::binary | std::ios::trunc);
        f << text;
        if (!f) return false;
    }
    std::error_code ec;
    fs::rename(rec::detail::pathFromUtf8(tmp), rec::detail::pathFromUtf8(p), ec);
    return !ec;
}

bool isAudioIsoFormat(const std::string& id) {
    rec::RecordingProfile p;
    rec::WriterFactory f;
    return rec::WriterRegistry::instance().find(id, p, f) && p.hasAudio && !p.hasVideo && id != "mp3";
}

AudioPlan audioPlanFor(const std::string& fmt) {
    AudioPlan a;
    if (fmt == "wav-16") a.bitsPerSample = 16;
    else if (fmt == "wav-f32") a.bitsPerSample = 32;
    else if (fmt == "wav") a.bitsPerSample = 24;
    else if (fmt.rfind("flac", 0) == 0) a.compressedKbps = fmt == "flac-16" ? 450 : 700;   // rough: ~half of PCM
    else a.compressedKbps = 192;                                                           // AAC
    return a;
}

const char* verdictName(core::BudgetVerdict v) {
    return v == core::BudgetVerdict::Ok ? "ok" : v == core::BudgetVerdict::Warning ? "warning" : "blocked";
}

}  // namespace

IsoService::IsoService(audio_io::AudioEngine& audio, video::ProgramOutput& program, core::EventBus* bus,
                       core::Logger* log, std::string dataDir, std::string appVersion)
    : audio_(audio), program_(program), bus_(bus), log_(log), dataDir_(std::move(dataDir)),
      appVersion_(std::move(appVersion)) {
    const core::HardwareInfo hw = core::detectHardware();
    tier_ = core::classify(hw);
    hwEncode_ = hw.hwEncode;
}

IsoService::~IsoService() { end(nullptr, ""); }

void IsoService::setProviders(Providers p) {
    std::lock_guard lk(mu_);
    prov_ = std::move(p);
}

void IsoService::setHardware(core::Tier tier, bool hwEncode) {
    std::lock_guard lk(mu_);
    tier_ = tier;
    hwEncode_ = hwEncode;
}

std::vector<IsoCamera> IsoService::cameras() const {
    std::function<std::vector<IsoCamera>()> fn;
    {
        std::lock_guard lk(mu_);
        fn = prov_.cameras;
    }
    try {
        return fn ? fn() : std::vector<IsoCamera>{};
    } catch (...) {
        return {};
    }
}

void IsoService::load() {
    if (dataDir_.empty()) return;
    const std::string text = readFile(dataDir_ + "/multitrack.json");
    js::Value v;
    if (text.empty() || !js::parse(text, v) || !v.isObj()) return;
    std::lock_guard lk(mu_);
    IsoSettings n;
    auto strv = [&](const char* k, std::string& dst, std::size_t maxLen) {
        if (auto* x = v.get(k); x && x->isStr() && x->s.size() <= maxLen) dst = x->s;
    };
    auto numv = [&](const char* k, int& dst, int lo, int hi) {
        if (auto* x = v.get(k); x && x->isNum() && std::isfinite(x->n)) dst = clampi(static_cast<int>(x->n), lo, hi);
    };
    if (auto* c = v.get("cams"); c && c->isArr())
        for (const auto& e : c->a) if (e.isNum() && e.n >= 0 && e.n < 64) n.cams.insert(static_cast<int>(e.n));
    if (auto* c = v.get("chans"); c && c->isArr())
        for (const auto& e : c->a) if (e.isNum() && e.n >= 0 && e.n < 64) n.chans.insert(static_cast<int>(e.n));
    strv("res", n.res, 16);
    strv("fps", n.fps, 16);
    strv("afmt", n.audioFormat, 32);
    numv("vkbps", n.videoKbps, 0, 100000);
    numv("rotmin", n.rotateMin, 0, 1440);
    if (auto* x = v.get("post"); x && x->isBool()) n.postFader = x->b;
    if (auto* x = v.get("expert"); x && x->isBool()) n.expert = x->b;
    if (n.res != "native" && n.res != "1080" && n.res != "720" && n.res != "540") n.res = "native";
    if (n.fps != "program" && n.fps != "24" && n.fps != "25" && n.fps != "30" && n.fps != "50" && n.fps != "60") n.fps = "program";
    s_ = n;
    ++ver_;
}

void IsoService::save() {
    if (dataDir_.empty()) return;
    IsoSettings s;
    { std::lock_guard lk(mu_); s = s_; }
    std::ostringstream o;
    o << "{\"cams\":[";
    bool f = true;
    for (int c : s.cams) { o << (f ? "" : ",") << c; f = false; }
    o << "],\"chans\":[";
    f = true;
    for (int c : s.chans) { o << (f ? "" : ",") << c; f = false; }
    o << "],\"res\":" << q(s.res) << ",\"fps\":" << q(s.fps) << ",\"vkbps\":" << s.videoKbps << ",\"afmt\":" << q(s.audioFormat)
      << ",\"post\":" << (s.postFader ? "true" : "false") << ",\"rotmin\":" << s.rotateMin << ",\"expert\":" << (s.expert ? "true" : "false") << "}";
    writeAtomic(dataDir_ + "/multitrack.json", o.str());
}

bool IsoService::enabled() const {
    std::lock_guard lk(mu_);
    return !s_.cams.empty() || !s_.chans.empty();
}

bool IsoService::active() const {
    std::lock_guard lk(mu_);
    return session_ != nullptr;
}

std::string IsoService::lastSessionDir() const {
    std::lock_guard lk(mu_);
    return lastDir_;
}

std::string IsoService::handle(const std::vector<std::string>& a, bool programRecording) {
    try {
        if (a.size() < 3) return "";
        if (programRecording || active()) return "Pare a gravação para mudar a seleção multipista.";
        const std::string& sub = a[2];
        int n = 0;
        {
            std::lock_guard lk(mu_);
            if ((sub == "cam" || sub == "ch") && a.size() >= 5 && toInt(a[3], n) && n >= 0 && n < 64) {
                auto& set = sub == "cam" ? s_.cams : s_.chans;
                if (a[4] == "1") set.insert(n); else set.erase(n);
            } else if (sub == "cfg" && a.size() >= 5) {
                const std::string& k = a[3];
                const std::string& val = a[4];
                if (k == "res") {
                    if (val != "native" && val != "1080" && val != "720" && val != "540") return "resolução inválida";
                    s_.res = val;
                } else if (k == "fps") {
                    if (val != "program" && val != "24" && val != "25" && val != "30" && val != "50" && val != "60") return "taxa inválida";
                    s_.fps = val;
                } else if (k == "vkbps") {
                    if (!toInt(val, n)) return "valor inválido";
                    s_.videoKbps = n <= 0 ? 0 : clampi(n, 1000, 100000);
                } else if (k == "afmt") {
                    if (!isAudioIsoFormat(val)) return "formato de áudio indisponível";
                    s_.audioFormat = val;
                } else if (k == "tap") {
                    if (val != "pre" && val != "post") return "ponto de captura inválido";
                    s_.postFader = val == "post";
                } else if (k == "rotmin") {
                    if (!toInt(val, n)) return "valor inválido";
                    s_.rotateMin = clampi(n, 0, 1440);
                } else if (k == "expert") {
                    s_.expert = val == "1";
                } else {
                    return "";
                }
            } else {
                return "";
            }
            ++ver_;
        }
        save();
    } catch (...) {
        return "erro interno ao processar o comando";
    }
    return "";
}

IsoService::Plan IsoService::buildPlan(const IsoSettings& s, int pn, int pd) const {
    Plan p;
    const auto cams = cameras();
    std::function<std::shared_ptr<const video::Source>(int)> srcFn;
    {
        std::lock_guard lk(mu_);
        srcFn = prov_.source;
    }
    const auto& pf = program_.format();
    for (int slot : s.cams) {
        const auto it = std::find_if(cams.begin(), cams.end(), [&](const IsoCamera& c) { return c.slot == slot; });
        if (it == cams.end() || !it->live) {
            p.notes.push_back("CAM " + std::to_string(slot + 1) + ": sem câmera ao vivo; não será gravada.");
            continue;
        }
        auto src = srcFn ? srcFn(slot) : nullptr;
        if (!src) {
            p.notes.push_back("CAM " + std::to_string(slot + 1) + ": fonte indisponível.");
            continue;
        }
        IsoVideoSpec v;
        v.slot = slot;
        v.name = it->device.empty() ? it->label : it->device;
        v.device = it->device;
        int w = it->width > 0 ? it->width : pf.width, h = it->height > 0 ? it->height : pf.height;
        if (s.res != "native") {
            const int th = s.res == "1080" ? 1080 : s.res == "720" ? 720 : 540;
            const double aspect = static_cast<double>(w) / std::max(1, h);
            h = th;
            w = static_cast<int>(std::lround(th * aspect));
        }
        if (w > 3840 || h > 2160) {   // never encode beyond 4K
            const double k = std::min(3840.0 / w, 2160.0 / h);
            w = static_cast<int>(w * k);
            h = static_cast<int>(h * k);
        }
        v.width = std::max(2, w & ~1);
        v.height = std::max(2, h & ~1);
        if (s.fps == "program") { v.fpsNum = pn; v.fpsDen = pd; }
        else { v.fpsNum = std::atoi(s.fps.c_str()); v.fpsDen = 1; }
        v.bitrateKbps = s.videoKbps;
        std::shared_ptr<const video::Source> keep = src;
        v.render = [keep](video::Frame& f, std::uint64_t idx) { keep->render(f, idx); };
        p.video.push_back(std::move(v));
    }
    const int strips = audio_.stripCount();
    const auto snap = audio_.snapshot();
    for (int strip : s.chans) {
        if (strip < 0 || strip >= strips) continue;
        IsoAudioSpec a;
        a.strip = strip;
        a.name = static_cast<std::size_t>(strip) < snap.strips.size() ? snap.strips[static_cast<std::size_t>(strip)].name : "CH " + std::to_string(strip + 1);
        a.profileId = s.audioFormat;
        a.postFader = s.postFader;
        p.audio.push_back(std::move(a));
    }
    return p;
}

bool IsoService::begin(const std::string& baseFolder, std::uint64_t programMinFree, double programKbps, int pn, int pd,
                       std::string& sessionDir, int& programSplitSeconds, std::string& error) {
    IsoSettings s;
    {
        std::lock_guard lk(mu_);
        if (session_) { error = "sessão multipista já em andamento"; return false; }
        s = s_;
    }
    programSplitSeconds = 0;
    Plan plan = buildPlan(s, pn, pd);
    if (plan.video.empty() && plan.audio.empty()) {
        error = "nenhuma trilha ISO válida: " + (plan.notes.empty() ? std::string("selecione câmeras ao vivo ou canais de áudio") : plan.notes.front());
        return false;
    }
    // Resource check before anything starts (same arithmetic the UI showed).
    EstimateInput ei;
    for (const auto& v : plan.video) ei.video.push_back({v.width, v.height, double(v.fpsNum) / std::max(1, v.fpsDen), v.bitrateKbps});
    for (size_t i = 0; i < plan.audio.size(); ++i) ei.audio.push_back(audioPlanFor(s.audioFormat));
    core::Tier tier; bool hw;
    { std::lock_guard lk(mu_); tier = tier_; hw = hwEncode_; }
    ei.tier = tier;
    ei.hwEncode = hw;
    ei.programKbps = programKbps;
    ei.expertOverride = s.expert;
    const std::uint64_t isoMinFree = programMinFree + (1500ull << 20);   // ISO stops 1.5 GB before the Program does
    std::error_code ec;
    const auto sp = fs::space(rec::detail::pathFromUtf8(baseFolder), ec);
    ei.freeBytes = ec ? 0 : static_cast<std::uint64_t>(sp.available);
    ei.minFreeBytes = isoMinFree;
    const Estimate est = estimate(ei);
    if (est.verdict == core::BudgetVerdict::Blocked) {
        error = est.notes.empty() ? "plano de gravação multipista acima do limite seguro" : est.notes.front();
        return false;
    }

    fs::create_directories(rec::detail::pathFromUtf8(baseFolder), ec);
    const std::time_t t = std::time(nullptr);
    std::tm lt{};
#ifdef _WIN32
    localtime_s(&lt, &t);
#else
    localtime_r(&t, &lt);
#endif
    const std::string name = rec::expandNameTemplate("{date}_{time}", lt, 0);
    sessionDir = rec::uniqueOutputPath(baseFolder, name, "");

    multitrack::IsoSessionConfig cfg;
    cfg.sessionDir = sessionDir;
    cfg.sessionName = name;
    cfg.appVersion = appVersion_;
    cfg.video = plan.video;
    cfg.audio = plan.audio;
    cfg.warnings = plan.notes;
    cfg.rotationSeconds = s.rotateMin * 60;
    cfg.minFreeBytes = isoMinFree;
    cfg.tcFpsNum = pn;
    cfg.tcFpsDen = pd;
    cfg.bus = bus_;
    cfg.log = log_;
    auto sess = std::make_shared<multitrack::IsoSession>(cfg);
    {
        std::lock_guard pk(progMu_);
        programFiles_.clear();
    }
    if (bus_)
        busSub_ = bus_->subscribe("recording.fileClosed", [this, dir = sessionDir](const core::Event& e) {
            auto k = e.fields.find("kind");
            auto p = e.fields.find("path");
            if (k == e.fields.end() || p == e.fields.end() || k->second != "program") return;
            if (p->second.rfind(dir, 0) != 0) return;
            std::lock_guard pk(progMu_);
            programFiles_.push_back(p->second);
        });
    if (!sess->start(error)) {
        if (bus_ && busSub_) bus_->unsubscribe(busSub_);
        busSub_ = 0;
        return false;
    }
    std::vector<audio_io::AudioEngine::ConsumerId> taps;
    std::weak_ptr<multitrack::IsoSession> weak = sess;
    for (size_t i = 0; i < plan.audio.size(); ++i) {
        const auto id = audio_.addStripConsumer(plan.audio[i].strip, plan.audio[i].postFader,
                                                [weak, i](const float* p, std::size_t n) {
                                                    if (auto ss = weak.lock()) ss->pushAudioNow(i, p, n);
                                                });
        if (id) taps.push_back(id);
    }
    {
        std::lock_guard lk(mu_);
        session_ = sess;
        taps_ = std::move(taps);
        lastDir_ = sessionDir;
        programSplitSeconds_ = cfg.rotationSeconds;
    }
    programSplitSeconds = cfg.rotationSeconds;
    if (log_) log_->info("multitrack", "sessão ISO iniciada: " + sessionDir);
    return true;
}

void IsoService::end(const rec::Recorder::Status* ps, const std::string& programProfileId) {
    std::shared_ptr<multitrack::IsoSession> sess;
    std::vector<audio_io::AudioEngine::ConsumerId> taps;
    int split = 0;
    {
        std::lock_guard lk(mu_);
        sess = std::move(session_);
        taps = std::move(taps_);
        split = programSplitSeconds_;
        session_.reset();
        taps_.clear();
    }
    if (!sess) return;
    for (auto id : taps) audio_.removeStripConsumer(id);   // synchronous: no more audio after this
    if (bus_ && busSub_) bus_->unsubscribe(busSub_);
    busSub_ = 0;

    ManifestTrack pt;
    pt.id = "program";
    pt.kind = "program";
    pt.name = "Program";
    pt.codec = programProfileId;
    const auto& pf = program_.format();
    pt.width = pf.width;
    pt.height = pf.height;
    pt.fpsNum = pf.fps.num;
    pt.fpsDen = pf.fps.den;
    pt.state = ps && !ps->error.empty() ? "error" : "finished";
    if (ps) {
        pt.error = ps->error;
        pt.units = ps->videoFrames;
        pt.repeated = ps->repeatedVideo;
        pt.droppedQueue = ps->droppedVideo;
        pt.silenceFrames = ps->silenceFrames;
        if (ps->t0Ns > 0) pt.startOffsetNs = sess->clock().toSession(ps->t0Ns);
    }
    std::vector<std::string> files;
    {
        std::lock_guard pk(progMu_);
        files = programFiles_;
    }
    if (files.empty() && ps && !ps->file.empty()) files.push_back(ps->file);
    for (size_t i = 0; i < files.size(); ++i) {
        ManifestSegment g;
        g.index = static_cast<int>(i) + 1;
        const auto cut = files[i].find_last_of("/\\");
        g.file = cut == std::string::npos ? files[i] : files[i].substr(cut + 1);
        // Later segments start where the Program recorder split them (media time); approximate to a frame.
        g.startNs = pt.startOffsetNs + static_cast<std::int64_t>(i) * split * multitrack::kNsPerSec;
        g.closed = true;
        pt.segments.push_back(g);
    }
    sess->setProgramTrack(pt);
    sess->stop();
    if (log_) log_->info("multitrack", "sessão ISO finalizada");
}

std::string IsoService::stateJson(bool programRecording, const std::string& baseFolder, double programKbps,
                                  std::uint64_t programMinFree) {
    IsoSettings s;
    std::shared_ptr<multitrack::IsoSession> sess;
    core::Tier tier;
    bool hw;
    {
        std::lock_guard lk(mu_);
        s = s_;
        sess = session_;
        tier = tier_;
        hw = hwEncode_;
    }
    const auto& pf = program_.format();
    Plan plan = buildPlan(s, pf.fps.num, pf.fps.den);
    EstimateInput ei;
    for (const auto& v : plan.video) ei.video.push_back({v.width, v.height, double(v.fpsNum) / std::max(1, v.fpsDen), v.bitrateKbps});
    for (size_t i = 0; i < plan.audio.size(); ++i) ei.audio.push_back(audioPlanFor(s.audioFormat));
    ei.tier = tier;
    ei.hwEncode = hw;
    ei.programKbps = programKbps;
    ei.expertOverride = s.expert;
    ei.minFreeBytes = programMinFree + (1500ull << 20);
    std::error_code ec;
    const auto sp = fs::space(rec::detail::pathFromUtf8(baseFolder), ec);
    ei.freeBytes = ec ? 0 : static_cast<std::uint64_t>(sp.available);
    const Estimate e = estimate(ei);

    std::ostringstream o;
    o << "{\"t\":\"rec_iso\",\"cfg\":{\"res\":" << q(s.res) << ",\"fps\":" << q(s.fps) << ",\"vkbps\":" << s.videoKbps
      << ",\"afmt\":" << q(s.audioFormat) << ",\"tap\":" << q(s.postFader ? "post" : "pre") << ",\"rotmin\":" << s.rotateMin
      << ",\"expert\":" << (s.expert ? "true" : "false") << "},\"locked\":" << ((programRecording || sess) ? "true" : "false")
      << ",\"cams\":[";
    bool first = true;
    for (const auto& c : cameras()) {
        if (!c.live && !s.cams.count(c.slot)) continue;
        o << (first ? "" : ",") << "{\"slot\":" << c.slot << ",\"label\":" << q(c.label) << ",\"device\":" << q(c.device)
          << ",\"live\":" << (c.live ? "true" : "false") << ",\"w\":" << c.width << ",\"h\":" << c.height << ",\"fps\":" << dbl(c.fps)
          << ",\"sel\":" << (s.cams.count(c.slot) ? "true" : "false") << "}";
        first = false;
    }
    o << "],\"chans\":[";
    const auto snap = audio_.snapshot();
    first = true;
    for (int i = 0; i < audio_.stripCount(); ++i) {
        const std::string nm = static_cast<std::size_t>(i) < snap.strips.size() ? snap.strips[static_cast<std::size_t>(i)].name : "CH " + std::to_string(i + 1);
        o << (first ? "" : ",") << "{\"strip\":" << i << ",\"name\":" << q(nm) << ",\"sel\":" << (s.chans.count(i) ? "true" : "false") << "}";
        first = false;
    }
    o << "],\"formats\":[";
    first = true;
    for (const auto& p : rec::WriterRegistry::instance().profiles()) {
        if (!(p.hasAudio && !p.hasVideo) || p.id == "mp3") continue;
        o << (first ? "" : ",") << "{\"id\":" << q(p.id) << ",\"label\":" << q(p.label) << "}";
        first = false;
    }
    o << "],\"est\":{\"video\":" << e.videoTracks << ",\"audio\":" << e.audioTracks << ",\"mbps\":" << dbl(e.totalMBps)
      << ",\"gbh\":" << dbl(e.gbPerHour) << ",\"load\":" << dbl(e.loadPercent) << ",\"maxCams\":" << e.maxVideoTracksSafe
      << ",\"minutes\":" << dbl(e.minutesOfDisk) << ",\"tier\":" << q(core::tierName(tier)) << ",\"verdict\":" << q(verdictName(e.verdict))
      << ",\"notes\":[";
    first = true;
    for (const auto& n : plan.notes) { o << (first ? "" : ",") << q(n); first = false; }
    for (const auto& n : e.notes) { o << (first ? "" : ",") << q(n); first = false; }
    o << "]},\"sess\":";
    if (!sess) {
        std::lock_guard lk(mu_);
        o << "{\"active\":false,\"dir\":" << q(lastDir_) << "}";
    } else {
        const auto st = sess->status();
        o << "{\"active\":" << (st.active ? "true" : "false") << ",\"seconds\":" << dbl(st.seconds) << ",\"free\":" << st.freeBytes
          << ",\"dir\":" << q(st.sessionDir) << ",\"tracks\":[";
        first = true;
        for (const auto& t : st.tracks) {
            o << (first ? "" : ",") << "{\"id\":" << q(t.id) << ",\"name\":" << q(t.name) << ",\"kind\":" << q(t.kind == multitrack::TrackKind::Video ? "video" : "audio")
              << ",\"state\":" << q(multitrack::trackStateName(t.state)) << ",\"error\":" << q(t.error) << ",\"seconds\":" << dbl(t.seconds)
              << ",\"segment\":" << t.segment << ",\"repeated\":" << t.repeated << ",\"dropped\":" << (t.droppedQueue + t.droppedTimeline)
              << ",\"silence\":" << t.silence << ",\"bytes\":" << t.bytes << ",\"drift\":" << dbl(t.driftPpm) << "}";
            first = false;
        }
        o << "],\"warnings\":[";
        first = true;
        for (const auto& w : st.warnings) { o << (first ? "" : ",") << q(w); first = false; }
        o << "]}";
    }
    o << "}";
    return o.str();
}

}  // namespace hb::app
