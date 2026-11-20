#include "audio_io/audio_engine.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#include "audio_io/engine_impl.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <timeapi.h>
#endif

namespace hb::audio_io {

using hb::audio::kProgramSampleRate;
using Clock = std::chrono::steady_clock;

const char* qualityName(Quality q) noexcept {
    switch (q) {
        case Quality::Linear: return "linear";
        case Quality::Medium: return "medium";
        case Quality::High: return "high";
    }
    return "medium";
}
const char* inputKindName(InputKind k) noexcept {
    switch (k) {
        case InputKind::None: return "none";
        case InputKind::Device: return "device";
        case InputKind::External: return "external";
    }
    return "none";
}

namespace {
hb::audio::ResamplerQuality toResamplerQuality(Quality q) {
    switch (q) {
        case Quality::Linear: return hb::audio::ResamplerQuality::Linear;
        case Quality::Medium: return hb::audio::ResamplerQuality::Medium;
        case Quality::High: return hb::audio::ResamplerQuality::High;
    }
    return hb::audio::ResamplerQuality::Medium;
}

const char* kDefaultNames[kMaxStrips] = {"MIC 1", "MIC 2", "LINHA", "SISTEMA", "FAIXA 5", "FAIXA 6", "FAIXA 7", "FAIXA 8"};

hb::audio::MixerConfig mixerConfig(int n) {
    hb::audio::MixerConfig c;
    for (int i = 0; i < n; ++i) c.channelNames.push_back(kDefaultNames[i]);
    c.maxBlockFrames = kBlockFrames;
    return c;
}
}  // namespace

const char* defaultStripName(int index) { return kDefaultNames[std::clamp(index, 0, kMaxStrips - 1)]; }

std::string sanitizeName(const std::string& s, std::size_t maxBytes) {
    std::string o;
    for (unsigned char c : s)
        if (c >= 0x20 && c != 0x7f) o.push_back(static_cast<char>(c));
    while (!o.empty() && o.front() == ' ') o.erase(o.begin());
    while (!o.empty() && o.back() == ' ') o.pop_back();
    if (o.size() > maxBytes) {
        std::size_t cut = maxBytes;
        while (cut > 0 && (static_cast<unsigned char>(o[cut]) & 0xC0) == 0x80) --cut;  // keep UTF-8 intact
        o.resize(cut);
    }
    return o;
}

// ------------------------------------------------------------------ InputNode
void InputNode::pull(float* out, std::size_t frames) noexcept {
    if (drift && session) {
        drift->pull(session->ring, out, frames);
    } else if (source) {
        if (!resampler) {
            source->pull(out, frames);
        } else {
            resampler->pull([&](float* d, std::size_t n) { source->pull(d, n); }, out, frames);
        }
    } else {
        std::memset(out, 0, frames * 2 * sizeof(float));
    }
}

StripRT::StripRT(int sr)
    : delay(sr, 2, 2000.0, 20.0),
      peak(hb::audio::PeakMeterConfig{sr, 1.5, 20.0}),
      rms(sr, 0.3),
      loud(sr),
      buf(kBlockFrames * 2, 0.0f) {
    for (auto& a : pub) a.store(hb::audio::kNegInfDb);
}

// ------------------------------------------------------------------ Impl
AudioEngine::Impl::Impl(EngineOptions o)
    : opt(std::move(o)),
      n(std::clamp(opt.strips, 1, kMaxStrips)),
      mixer(mixerConfig(n)),
      enumerator(opt.backend, opt.lister),
      monPeak(hb::audio::PeakMeterConfig{kProgramSampleRate, 1.5, 20.0}),
      monBuf(kBlockFrames * 2, 0.0f),
      prog(kBlockFrames * 2, 0.0f) {
    for (int i = 0; i < n; ++i) {
        rt.push_back(std::make_unique<StripRT>(kProgramSampleRate));
        StripCtl c;
        c.name = kDefaultNames[i];
        c.chain = std::make_shared<Chain>();
        rt.back()->chain.store(c.chain.get());
        strips.push_back(std::move(c));
    }
    for (auto& a : monPub) a.store(hb::audio::kNegInfDb);
    consOwner = std::make_shared<ConsumerList>();
    consPub.store(consOwner.get());
}

bool AudioEngine::Impl::fail(const std::string& msg) {
    std::lock_guard lock(errMutex);
    lastError = msg;
    return false;
}

bool AudioEngine::Impl::checkStrip(int i) {
    if (i < 0 || i >= n) return fail("Faixa inválida: " + std::to_string(i) + ".");
    return true;
}

void AudioEngine::Impl::retire(std::shared_ptr<const void> p) {
    if (!p) return;
    std::lock_guard lock(retMutex);
    retired.emplace_back(gate.mark(), std::move(p));  // mark is taken after the swap
}

void AudioEngine::Impl::collect() {
    std::vector<std::shared_ptr<const void>> dead;  // destroyed outside the lock
    {
        std::lock_guard lock(retMutex);
        for (auto it = retired.begin(); it != retired.end();) {
            if (gate.passed(it->first)) {
                dead.push_back(std::move(it->second));
                it = retired.erase(it);
            } else {
                ++it;
            }
        }
    }
}

void AudioEngine::Impl::rebuildInput(int i) {
    StripCtl& c = strips[static_cast<std::size_t>(i)];
    std::shared_ptr<InputNode> node;
    if (c.kind == InputKind::Device && c.capture) {
        node = std::make_shared<InputNode>();
        node->stream = c.capture;
        node->session = c.capture->session();
        if (node->session) {
            const int sr = node->session->sampleRate;
            node->drift = std::make_unique<hb::audio::DriftCompensator>(
                sr, 2, static_cast<std::size_t>(sr) * static_cast<std::size_t>(std::max(10, opt.captureTargetMs)) / 1000,
                toResamplerQuality(c.quality));
        }
    } else if (c.kind == InputKind::External) {
        auto it = externals.find(c.externalId);
        if (it != externals.end() && it->second) {
            node = std::make_shared<InputNode>();
            node->source = it->second;
            const int sr = it->second->sampleRate();
            if (sr != kProgramSampleRate && sr >= 8000 && sr <= 384000)
                node->resampler = std::make_unique<hb::audio::Resampler>(sr, kProgramSampleRate, 2,
                                                                         toResamplerQuality(c.quality));
        }
    }
    rt[static_cast<std::size_t>(i)]->input.store(node.get());
    std::shared_ptr<InputNode> old = std::move(c.node);
    c.node = std::move(node);
    retire(std::move(old));
}

void AudioEngine::Impl::publishChain(int i, std::shared_ptr<Chain> chain) {
    StripCtl& c = strips[static_cast<std::size_t>(i)];
    rt[static_cast<std::size_t>(i)]->chain.store(chain.get());
    std::shared_ptr<Chain> old = std::move(c.chain);
    c.chain = std::move(chain);
    retire(std::move(old));
}

void AudioEngine::Impl::applyStripToRt(int i) {
    const StripCtl& c = strips[static_cast<std::size_t>(i)];
    StripRT& r = *rt[static_cast<std::size_t>(i)];
    r.trim.store(hb::audio::dbToLinear(c.trimDb));
    r.phase.store(c.phase);
    r.mono.store(c.mono);
    r.toProgram.store(c.toProgram);
    r.toMonitor.store(c.toMonitor);
    r.delay.setDelayMs(c.delayMs);
    const std::size_t u = static_cast<std::size_t>(i);
    mixer.setGainDb(u, 0.0f);
    mixer.setFaderDb(u, c.faderDb);
    mixer.setMute(u, c.mute);
    mixer.setSolo(u, c.solo);
    mixer.setPan(u, c.pan);
}

// ------------------------------------------------------------------ lifecycle
AudioEngine::AudioEngine(EngineOptions options) : p_(std::make_unique<Impl>(std::move(options))) {}

AudioEngine::~AudioEngine() {
    stop();
    std::vector<std::shared_ptr<CaptureStream>> caps;
    std::shared_ptr<PlaybackStream> mon;
    {
        std::lock_guard lock(p_->ctl);
        for (auto& s : p_->strips) caps.push_back(s.capture);
        mon = p_->mon.stream;
    }
    for (auto& c : caps)
        if (c) c->stop();
    if (mon) mon->stop();
}

bool AudioEngine::running() const { return p_->isRunning.load(); }

void AudioEngine::start() {
    Impl& P = *p_;
    std::lock_guard lock(P.runMutex);
    if (P.isRunning.load()) return;
    P.stopFlag.store(false);
    P.isRunning.store(true);
    P.engineThread = std::thread([this] { p_->engineLoop(); });
    P.maintThread = std::thread([this] { p_->maintLoop(); });
}

void AudioEngine::stop() {
    Impl& P = *p_;
    std::lock_guard lock(P.runMutex);
    if (!P.isRunning.load()) return;
    {
        std::lock_guard wl(P.wakeMutex);
        P.stopFlag.store(true);
    }
    P.wake.notify_all();
    if (P.engineThread.joinable()) P.engineThread.join();
    if (P.maintThread.joinable()) P.maintThread.join();
    P.isRunning.store(false);
}

void AudioEngine::Impl::maintLoop() {
    for (;;) {
        {
            std::unique_lock lock(wakeMutex);
            if (wake.wait_for(lock, std::chrono::milliseconds(50), [&] { return stopFlag.load(); })) return;
        }
        try {
            maintenanceStep(*this);
        } catch (...) {
        }
    }
}

void AudioEngine::Impl::engineLoop() {
#if defined(_WIN32)
    timeBeginPeriod(1);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
#endif
    const auto period = std::chrono::milliseconds(10);
    auto next = Clock::now();
    while (!stopFlag.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_until(next);
        if (stopFlag.load(std::memory_order_relaxed)) break;
        processBlockImpl(*this);
        next += period;
        const auto now = Clock::now();
        if (now > next + period) {  // behind by more than one block
            xruns.fetch_add(1, std::memory_order_relaxed);
            if (now > next + 20 * period) next = now;  // hopeless: resynchronise instead of racing
        }
    }
#if defined(_WIN32)
    timeEndPeriod(1);
#endif
}

// ------------------------------------------------------------------ real-time block
namespace {

void processStrip(StripRT& s, std::size_t frames) noexcept {
    float* b = s.buf.data();
    if (InputNode* in = s.input.load()) in->pull(b, frames);
    else std::memset(b, 0, frames * 2 * sizeof(float));

    const float tgt = s.trim.load(std::memory_order_relaxed) * (s.phase.load(std::memory_order_relaxed) ? -1.0f : 1.0f);
    const float mt = s.mono.load(std::memory_order_relaxed) ? 1.0f : 0.0f;
    if (!s.init) {
        s.trimCur = tgt;
        s.monoCur = mt;
        s.init = true;
    }
    if (s.trimCur == tgt && s.monoCur == mt) {
        if (mt == 0.0f) {
            if (tgt != 1.0f)
                for (std::size_t i = 0; i < frames * 2; ++i) b[i] *= tgt;
        } else {
            for (std::size_t i = 0; i < frames; ++i) {
                const float mid = 0.5f * (b[2 * i] + b[2 * i + 1]) * tgt;
                b[2 * i] = b[2 * i + 1] = mid;
            }
        }
    } else {  // parameter changed: linear ramp over the block (no zipper noise)
        const float t0 = s.trimCur, m0 = s.monoCur;
        const float inv = 1.0f / static_cast<float>(frames);
        for (std::size_t i = 0; i < frames; ++i) {
            const float k = static_cast<float>(i + 1) * inv;
            const float t = t0 + (tgt - t0) * k, m = m0 + (mt - m0) * k;
            const float l = b[2 * i], r = b[2 * i + 1];
            const float mid = 0.5f * (l + r);
            b[2 * i] = (l + (mid - l) * m) * t;
            b[2 * i + 1] = (r + (mid - r) * m) * t;
        }
        s.trimCur = tgt;
        s.monoCur = mt;
    }

    s.delay.process(b, frames);

    float gr = 0.0f;
    if (const Chain* ch = s.chain.load()) {
        for (const auto& slot : ch->slots) {
            Slot* sl = slot.get();
            if (!sl->fx || sl->bypass.load(std::memory_order_relaxed)) {
                sl->gr.store(0.0f, std::memory_order_relaxed);
                continue;
            }
            sl->fx->process(b, frames);
            const float g = sl->fx->gainReductionDb();
            const float gg = std::isfinite(g) && g > 0.0f ? g : 0.0f;
            sl->gr.store(gg, std::memory_order_relaxed);
            gr += gg;
        }
    }

    s.peak.process(b, frames);
    s.rms.process(b, frames);
    s.loud.process(b, frames);
    s.pub[0].store(s.peak.levelDb(0), std::memory_order_relaxed);
    s.pub[1].store(s.peak.levelDb(1), std::memory_order_relaxed);
    s.pub[2].store(s.peak.holdDb(0), std::memory_order_relaxed);
    s.pub[3].store(s.peak.holdDb(1), std::memory_order_relaxed);
    s.pub[4].store(s.rms.rmsDb(0), std::memory_order_relaxed);
    s.pub[5].store(s.rms.rmsDb(1), std::memory_order_relaxed);
    s.pub[6].store(s.loud.shortTermLufs(), std::memory_order_relaxed);
    s.pub[7].store(gr, std::memory_order_relaxed);
}

}  // namespace

void processBlockImpl(AudioEngine::Impl& P) {
    const auto t0 = Clock::now();
    const std::size_t F = kBlockFrames;
    P.engineThreadId.store(std::this_thread::get_id(), std::memory_order_relaxed);
    P.gate.enter();

    std::array<const float*, kMaxStrips> ptrs{};
    PlaybackStream* ms = P.monStream.load();
    if (ms) std::fill(P.monBuf.begin(), P.monBuf.end(), 0.0f);
    for (int i = 0; i < P.n; ++i) {
        StripRT& s = *P.rt[static_cast<std::size_t>(i)];
        processStrip(s, F);
        ptrs[static_cast<std::size_t>(i)] = s.toProgram.load(std::memory_order_relaxed) ? s.buf.data() : nullptr;
        if (ms && s.toMonitor.load(std::memory_order_relaxed))
            for (std::size_t k = 0; k < F * 2; ++k) P.monBuf[k] += s.buf[k];
    }
    P.mixer.process(ptrs.data(), P.prog.data(), F);

    if (ms) {
        const float tgt = P.monMute.load(std::memory_order_relaxed) ? 0.0f : P.monGain.load(std::memory_order_relaxed);
        if (!P.monInit) {
            P.monCur = tgt;
            P.monInit = true;
        }
        const float g0 = P.monCur, inv = 1.0f / static_cast<float>(F);
        for (std::size_t i = 0; i < F; ++i) {
            const float g = g0 + (tgt - g0) * static_cast<float>(i + 1) * inv;
            P.monBuf[2 * i] *= g;
            P.monBuf[2 * i + 1] *= g;
        }
        P.monCur = tgt;
        P.monPeak.process(P.monBuf.data(), F);
        P.monPub[0].store(P.monPeak.levelDb(0), std::memory_order_relaxed);
        P.monPub[1].store(P.monPeak.levelDb(1), std::memory_order_relaxed);
        ms->write(P.monBuf.data(), F);
    }

    if (const ConsumerList* l = P.consPub.load()) {
        for (const auto& e : l->items) {
            try {
                e->fn(P.prog.data(), F);
            } catch (...) {
                P.consumerErrors.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    P.gate.exit();

    P.blocks.fetch_add(1, std::memory_order_relaxed);
    const double pct = std::chrono::duration<double, std::milli>(Clock::now() - t0).count() / 10.0 * 100.0;
    const double prev = P.cpu.load(std::memory_order_relaxed);
    P.cpu.store(prev + 0.05 * (pct - prev), std::memory_order_relaxed);
    if (pct > P.cpuPeak.load(std::memory_order_relaxed)) P.cpuPeak.store(pct, std::memory_order_relaxed);
}

void maintenanceStep(AudioEngine::Impl& P) {
    std::lock_guard lock(P.ctl);
    for (int i = 0; i < P.n; ++i) {
        StripCtl& c = P.strips[static_cast<std::size_t>(i)];
        if (c.capture) {
            if (!P.opt.streamSupervisors) c.capture->tick();
            auto s = c.capture->session();
            if (c.kind == InputKind::Device && (!c.node || c.node->session != s)) P.rebuildInput(i);
        }
    }
    if (P.mon.stream && !P.opt.streamSupervisors) P.mon.stream->tick();
    P.collect();
}

void AudioEngine::processBlock() { processBlockImpl(*p_); }
void AudioEngine::runMaintenance() { maintenanceStep(*p_); }

// ------------------------------------------------------------------ strip commands
namespace {
bool isNum(double v) { return !std::isnan(v); }
}  // namespace

bool AudioEngine::setStripParam(int strip, const std::string& key, double v) {
    Impl& P = *p_;
    std::lock_guard lock(P.ctl);
    if (!P.checkStrip(strip)) return false;
    if (!isNum(v)) return P.fail("Valor inválido (NaN) para '" + key + "'.");
    StripCtl& c = P.strips[static_cast<std::size_t>(strip)];
    const bool b = v != 0.0;
    if (key == "trimDb") c.trimDb = static_cast<float>(std::clamp(v, -60.0, 24.0));
    else if (key == "faderDb") c.faderDb = v <= -100.0 ? hb::audio::kNegInfDb : static_cast<float>(std::min(v, 10.0));
    else if (key == "mute") c.mute = b;
    else if (key == "solo") c.solo = b;
    else if (key == "pan") c.pan = static_cast<float>(std::clamp(v, -1.0, 1.0));
    else if (key == "phase") c.phase = b;
    else if (key == "mono") c.mono = b;
    else if (key == "delayMs") c.delayMs = static_cast<float>(std::clamp(v, 0.0, 2000.0));
    else if (key == "toProgram") c.toProgram = b;
    else if (key == "toMonitor") c.toMonitor = b;
    else return P.fail("Parâmetro de faixa desconhecido: '" + key + "'.");
    P.applyStripToRt(strip);
    P.version.fetch_add(1);
    return true;
}

bool AudioEngine::setStripParam(int strip, const std::string& key, const std::string& value) {
    Impl& P = *p_;
    std::lock_guard lock(P.ctl);
    if (!P.checkStrip(strip)) return false;
    if (key != "name") return P.fail("O parâmetro '" + key + "' não aceita texto.");
    const std::string nm = sanitizeName(value, 32);
    if (nm.empty()) return P.fail("O nome da faixa não pode ficar vazio.");
    P.strips[static_cast<std::size_t>(strip)].name = nm;
    P.version.fetch_add(1);
    return true;
}

bool AudioEngine::setStripQuality(int strip, const std::string& quality) {
    Impl& P = *p_;
    std::lock_guard lock(P.ctl);
    if (!P.checkStrip(strip)) return false;
    Quality q;
    if (quality == "linear") q = Quality::Linear;
    else if (quality == "medium") q = Quality::Medium;
    else if (quality == "high") q = Quality::High;
    else return P.fail("Qualidade desconhecida: '" + quality + "' (use linear, medium ou high).");
    StripCtl& c = P.strips[static_cast<std::size_t>(strip)];
    if (c.quality != q) {
        c.quality = q;
        if (c.kind != InputKind::None) P.rebuildInput(strip);
    }
    P.version.fetch_add(1);
    return true;
}

bool AudioEngine::setStripInput(int strip, const std::string& kind, const std::string& id) {
    Impl& P = *p_;
    std::lock_guard lock(P.ctl);
    if (!P.checkStrip(strip)) return false;
    StripCtl& c = P.strips[static_cast<std::size_t>(strip)];
    if (id.size() > 600) return P.fail("Identificador de dispositivo longo demais.");
    std::shared_ptr<CaptureStream> oldCapture;
    if (kind == "none") {
        oldCapture = std::move(c.capture);
        c.kind = InputKind::None;
        c.deviceId.clear();
        c.externalId.clear();
    } else if (kind == "device") {
        if (id.empty()) return P.fail("Escolha um dispositivo de áudio.");
        if (id.rfind("play:", 0) == 0) return P.fail("Dispositivos de reprodução não podem ser usados como entrada.");
        if (!(c.kind == InputKind::Device && c.deviceId == id && c.capture)) {
            oldCapture = std::move(c.capture);
            StreamOptions so;
            so.backend = P.opt.backend;
            so.opener = P.opt.opener;
            so.clockNs = P.opt.clockNs;
            so.supervisor = P.opt.streamSupervisors;
            c.capture = std::make_shared<CaptureStream>(
                id, id.rfind("loop:", 0) == 0 ? DeviceKind::Loopback : DeviceKind::Capture, so);
            c.kind = InputKind::Device;
            c.deviceId = id;
            c.externalId.clear();
            c.capture->start();
        }
    } else if (kind == "external") {
        if (id.empty()) return P.fail("Informe o identificador da fonte externa.");
        oldCapture = std::move(c.capture);
        c.kind = InputKind::External;
        c.externalId = id;
        c.deviceId.clear();
    } else {
        return P.fail("Tipo de entrada desconhecido: '" + kind + "'.");
    }
    P.rebuildInput(strip);
    if (oldCapture) oldCapture->stop();  // releases the device (the retired node keeps the object alive)
    P.version.fetch_add(1);
    return true;
}

bool AudioEngine::registerExternalSource(const std::string& id, std::shared_ptr<hb::audio::AudioSource> source) {
    Impl& P = *p_;
    std::lock_guard lock(P.ctl);
    if (id.empty() || id.size() > 200 || !source) return P.fail("Fonte externa inválida.");
    P.externals[id] = std::move(source);
    for (int i = 0; i < P.n; ++i) {
        const StripCtl& c = P.strips[static_cast<std::size_t>(i)];
        if (c.kind == InputKind::External && c.externalId == id) P.rebuildInput(i);
    }
    P.version.fetch_add(1);
    return true;
}

void AudioEngine::unregisterExternalSource(const std::string& id) {
    Impl& P = *p_;
    std::lock_guard lock(P.ctl);
    if (P.externals.erase(id) == 0) return;
    for (int i = 0; i < P.n; ++i) {
        const StripCtl& c = P.strips[static_cast<std::size_t>(i)];
        if (c.kind == InputKind::External && c.externalId == id) P.rebuildInput(i);
    }
    P.version.fetch_add(1);
}

// ------------------------------------------------------------------ effects
bool AudioEngine::addEffect(int strip, const std::string& typeId, int pos) {
    Impl& P = *p_;
    std::lock_guard lock(P.ctl);
    if (!P.checkStrip(strip)) return false;
    StripCtl& c = P.strips[static_cast<std::size_t>(strip)];
    if (c.chain->slots.size() >= static_cast<std::size_t>(kMaxEffects))
        return P.fail("A cadeia de efeitos já tem o máximo de " + std::to_string(kMaxEffects) + " efeitos.");
    auto fx = hb::audio::EffectRegistry::instance().create(typeId);
    if (!fx) return P.fail("Efeito desconhecido ou indisponível: '" + typeId + "'.");
    try {
        fx->prepare(kProgramSampleRate, 2);
    } catch (...) {
        return P.fail("O efeito '" + typeId + "' falhou ao inicializar.");
    }
    auto slot = std::make_shared<Slot>();
    slot->typeId = typeId;
    for (const auto& i : hb::audio::EffectRegistry::instance().list())
        if (i.typeId == typeId) slot->name = i.name;
    slot->fx = std::shared_ptr<hb::audio::Effect>(std::move(fx));
    auto chain = std::make_shared<Chain>(*c.chain);
    const std::size_t at = pos < 0 || static_cast<std::size_t>(pos) > chain->slots.size()
                               ? chain->slots.size()
                               : static_cast<std::size_t>(pos);
    chain->slots.insert(chain->slots.begin() + static_cast<std::ptrdiff_t>(at), std::move(slot));
    P.publishChain(strip, std::move(chain));
    P.version.fetch_add(1);
    return true;
}

bool AudioEngine::removeEffect(int strip, int idx) {
    Impl& P = *p_;
    std::lock_guard lock(P.ctl);
    if (!P.checkStrip(strip)) return false;
    StripCtl& c = P.strips[static_cast<std::size_t>(strip)];
    if (idx < 0 || static_cast<std::size_t>(idx) >= c.chain->slots.size()) return P.fail("Índice de efeito inválido.");
    auto chain = std::make_shared<Chain>(*c.chain);
    chain->slots.erase(chain->slots.begin() + idx);
    P.publishChain(strip, std::move(chain));
    P.version.fetch_add(1);
    return true;
}

bool AudioEngine::moveEffect(int strip, int from, int to) {
    Impl& P = *p_;
    std::lock_guard lock(P.ctl);
    if (!P.checkStrip(strip)) return false;
    StripCtl& c = P.strips[static_cast<std::size_t>(strip)];
    const int sz = static_cast<int>(c.chain->slots.size());
    if (from < 0 || from >= sz || to < 0 || to >= sz) return P.fail("Índice de efeito inválido.");
    if (from == to) return true;
    auto chain = std::make_shared<Chain>(*c.chain);
    auto s = chain->slots[static_cast<std::size_t>(from)];
    chain->slots.erase(chain->slots.begin() + from);
    chain->slots.insert(chain->slots.begin() + to, std::move(s));
    P.publishChain(strip, std::move(chain));
    P.version.fetch_add(1);
    return true;
}

bool AudioEngine::setEffectBypass(int strip, int idx, bool bypass) {
    Impl& P = *p_;
    std::lock_guard lock(P.ctl);
    if (!P.checkStrip(strip)) return false;
    StripCtl& c = P.strips[static_cast<std::size_t>(strip)];
    if (idx < 0 || static_cast<std::size_t>(idx) >= c.chain->slots.size()) return P.fail("Índice de efeito inválido.");
    c.chain->slots[static_cast<std::size_t>(idx)]->bypass.store(bypass);  // atomic inside the shared slot
    P.version.fetch_add(1);
    return true;
}

bool AudioEngine::setEffectParam(int strip, int idx, const std::string& paramId, double value) {
    Impl& P = *p_;
    std::lock_guard lock(P.ctl);
    if (!P.checkStrip(strip)) return false;
    StripCtl& c = P.strips[static_cast<std::size_t>(strip)];
    if (idx < 0 || static_cast<std::size_t>(idx) >= c.chain->slots.size()) return P.fail("Índice de efeito inválido.");
    if (!isNum(value)) return P.fail("Valor inválido (NaN).");
    Slot& s = *c.chain->slots[static_cast<std::size_t>(idx)];
    if (!s.fx) return P.fail("O efeito '" + s.typeId + "' está ausente (plugin não instalado).");
    for (const auto& info : s.fx->params()) {
        if (info.id != paramId) continue;
        double v = std::clamp(value, static_cast<double>(info.min), static_cast<double>(info.max));
        if (!info.choices.empty()) v = std::clamp(std::round(v), 0.0, static_cast<double>(info.choices.size() - 1));
        s.fx->setParam(paramId, static_cast<float>(v));
        P.version.fetch_add(1);
        return true;
    }
    return P.fail("Parâmetro desconhecido '" + paramId + "' em " + s.typeId + ".");
}

// ------------------------------------------------------------------ master / monitor
bool AudioEngine::setMasterParam(const std::string& key, double v) {
    Impl& P = *p_;
    std::lock_guard lock(P.ctl);
    if (!isNum(v)) return P.fail("Valor inválido (NaN) para '" + key + "'.");
    if (key == "faderDb") {
        P.master.faderDb = v <= -100.0 ? hb::audio::kNegInfDb : static_cast<float>(std::min(v, 10.0));
        P.mixer.setMasterFaderDb(P.master.faderDb);
    } else if (key == "limiter") {
        P.master.limiter = v != 0.0;
        P.mixer.setLimiterMode(P.master.limiter ? hb::audio::LimiterMode::Limiter : hb::audio::LimiterMode::Off);
    } else if (key == "ceilingDb") {
        P.master.ceilingDb = static_cast<float>(std::clamp(v, -12.0, 0.0));
        P.mixer.setLimiterCeilingDb(P.master.ceilingDb);
    } else {
        return P.fail("Parâmetro do master desconhecido: '" + key + "'.");
    }
    P.version.fetch_add(1);
    return true;
}

bool AudioEngine::setMonitor(const std::string& key, double v) {
    Impl& P = *p_;
    std::lock_guard lock(P.ctl);
    if (!isNum(v)) return P.fail("Valor inválido (NaN) para '" + key + "'.");
    if (key == "level") {
        P.mon.level = static_cast<float>(std::clamp(v, 0.0, 1.0));
        P.monGain.store(hb::audio::dbToLinear(hb::audio::faderPositionToDb(P.mon.level)));
    } else if (key == "mute") {
        P.mon.mute = v != 0.0;
        P.monMute.store(P.mon.mute);
    } else {
        return P.fail("Parâmetro de monitoramento desconhecido: '" + key + "'.");
    }
    P.version.fetch_add(1);
    return true;
}

bool AudioEngine::setMonitor(const std::string& key, const std::string& value) {
    Impl& P = *p_;
    std::lock_guard lock(P.ctl);
    if (key != "deviceId") return P.fail("O parâmetro '" + key + "' não aceita texto.");
    if (value.size() > 600) return P.fail("Identificador de dispositivo longo demais.");
    if (!value.empty() && value.rfind("play:", 0) != 0) return P.fail("Escolha um dispositivo de reprodução.");
    if (value == P.mon.deviceId && (value.empty() || P.mon.stream)) return true;
    std::shared_ptr<PlaybackStream> old = std::move(P.mon.stream);
    P.mon.deviceId = value;
    if (value.empty()) {
        P.monStream.store(nullptr);
    } else {
        StreamOptions so;
        so.backend = P.opt.backend;
        so.opener = P.opt.opener;
        so.clockNs = P.opt.clockNs;
        so.supervisor = P.opt.streamSupervisors;
        P.mon.stream = std::make_shared<PlaybackStream>(value, so, P.opt.monitorLatencyMs);
        P.monStream.store(P.mon.stream.get());
        P.mon.stream->start();
    }
    if (old) {
        old->stop();
        P.retire(std::move(old));
    }
    P.version.fetch_add(1);
    return true;
}

bool AudioEngine::refreshDevices() {
    const bool ok = p_->enumerator.refresh();
    if (!ok) {
        std::lock_guard lock(p_->errMutex);
        p_->lastError = p_->enumerator.lastError();
    }
    p_->version.fetch_add(1);
    return ok;
}

void AudioEngine::resetMeters() { p_->mixer.resetMeters(); }

// ------------------------------------------------------------------ consumers
AudioEngine::ConsumerId AudioEngine::addProgramConsumer(ProgramConsumer consumer) {
    Impl& P = *p_;
    if (!consumer) return 0;
    std::lock_guard lock(P.consMutex);
    auto e = std::make_shared<ConsumerEntry>();
    e->id = P.nextConsumer++;
    e->fn = std::move(consumer);
    auto list = std::make_shared<ConsumerList>(*P.consOwner);
    list->items.push_back(e);
    P.consPub.store(list.get());
    auto old = std::move(P.consOwner);
    P.consOwner = std::move(list);
    P.retire(std::move(old));
    return e->id;
}

void AudioEngine::removeProgramConsumer(ConsumerId id) {
    Impl& P = *p_;
    std::shared_ptr<ConsumerEntry> removed;
    std::uint64_t mark = 0;
    {
        std::lock_guard lock(P.consMutex);
        auto list = std::make_shared<ConsumerList>();
        for (const auto& e : P.consOwner->items) {
            if (e->id == id) removed = e;
            else list->items.push_back(e);
        }
        if (!removed) return;
        P.consPub.store(list.get());
        auto old = std::move(P.consOwner);
        P.consOwner = std::move(list);
        P.retire(std::move(old));  // still references `removed`: freed only after the block ends
        mark = P.gate.mark();
    }
    if (P.engineThreadId.load() == std::this_thread::get_id()) return;  // removing itself from its callback
    while (!P.gate.passed(mark)) std::this_thread::sleep_for(std::chrono::microseconds(100));
    removed.reset();
}

// ------------------------------------------------------------------ state
unsigned AudioEngine::version() const { return p_->version.load(); }
std::string AudioEngine::lastError() const {
    std::lock_guard lock(p_->errMutex);
    return p_->lastError;
}
int AudioEngine::stripCount() const { return p_->n; }

std::shared_ptr<hb::audio::Effect> AudioEngine::effectInstance(int strip, int idx) const {
    Impl& P = *p_;
    std::lock_guard lock(P.ctl);
    if (!P.checkStrip(strip) || idx < 0) return nullptr;
    const StripCtl& c = P.strips[static_cast<std::size_t>(strip)];
    if (!c.chain || idx >= static_cast<int>(c.chain->slots.size())) return nullptr;
    return c.chain->slots[static_cast<std::size_t>(idx)]->fx;
}

EngineStats AudioEngine::stats() const {
    Impl& P = *p_;
    EngineStats s;
    s.blocks = P.blocks.load();
    s.xruns = P.xruns.load();
    s.consumerErrors = P.consumerErrors.load();
    s.cpuPercent = P.cpu.load();
    s.cpuPeakPercent = P.cpuPeak.load();
    std::lock_guard lock(P.ctl);
    for (const auto& c : P.strips) {
        if (!c.capture) continue;
        if (auto ses = c.capture->session()) {
            s.underruns += ses->ring.underruns();
            s.overruns += ses->ring.overruns();
        }
    }
    if (P.mon.stream) {
        const auto st = P.mon.stream->status();
        s.underruns += st.underruns;
        s.overruns += st.overruns;
    }
    return s;
}

}  // namespace hb::audio_io
