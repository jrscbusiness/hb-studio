#include "audio/mixer.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace hb::audio {

// ------------------------------------------------------------- fader / pan
namespace {
struct Knot { float pos, db; };
constexpr Knot kKnots[] = {{0.10f, -60.0f}, {0.25f, -40.0f}, {0.50f, -20.0f}, {0.75f, 0.0f}, {1.00f, 10.0f}};
constexpr int kNumKnots = 5;
}  // namespace

float faderPositionToDb(float pos) noexcept {
    if (!(pos > 0.0f)) return kNegInfDb;
    pos = std::min(pos, 1.0f);
    if (pos < kKnots[0].pos) return linearToDb(dbToLinear(kKnots[0].db) * pos / kKnots[0].pos);
    for (int i = 1; i < kNumKnots; ++i) {
        if (pos <= kKnots[i].pos) {
            const float t = (pos - kKnots[i - 1].pos) / (kKnots[i].pos - kKnots[i - 1].pos);
            return kKnots[i - 1].db + t * (kKnots[i].db - kKnots[i - 1].db);
        }
    }
    return kFaderMaxDb;
}

float dbToFaderPosition(float db) noexcept {
    if (!(db > kNegInfDb)) return 0.0f;
    if (db >= kFaderMaxDb) return 1.0f;
    if (db < kKnots[0].db) return std::max(0.0f, dbToLinear(db) / dbToLinear(kKnots[0].db) * kKnots[0].pos);
    for (int i = 1; i < kNumKnots; ++i) {
        if (db <= kKnots[i].db) {
            const float t = (db - kKnots[i - 1].db) / (kKnots[i].db - kKnots[i - 1].db);
            return kKnots[i - 1].pos + t * (kKnots[i].pos - kKnots[i - 1].pos);
        }
    }
    return 1.0f;
}

void panGains(float pan, float& l, float& r) noexcept {
    pan = std::clamp(pan, -1.0f, 1.0f);
    const float theta = (pan + 1.0f) * static_cast<float>(kPi) * 0.25f;  // 0 .. pi/2
    l = std::cos(theta);
    r = std::sin(theta);
}

MixerConfig MixerConfig::broadcastDefault(int cameras) {
    MixerConfig c;
    for (int i = 1; i <= cameras; ++i) c.channelNames.push_back("CAM " + std::to_string(i));
    c.channelNames.push_back("MIC 1");
    c.channelNames.push_back("MIC 2");
    c.channelNames.push_back("SYSTEM");
    return c;
}

// ----------------------------------------------------------------- channel
struct Mixer::Channel {
    // written by the UI thread
    std::atomic<float> gainDb{0.0f}, faderDb{0.0f}, gainLin{1.0f}, faderLin{1.0f};
    std::atomic<float> pan{0.0f}, panL{0.70710678f}, panR{0.70710678f};
    std::atomic<bool> mute{false}, solo{false};
    std::atomic<AudioSource*> src{nullptr};
    // audio thread only
    float curL = 0, curR = 0, tgtL = 0, tgtR = 0, stepL = 0, stepR = 0;
    std::size_t remaining = 0;
    bool primed = false;
    PeakMeter meter;
    // published
    std::array<std::atomic<float>, 4> lv;
    explicit Channel(int sr) : meter(PeakMeterConfig{sr, 1.5, 20.0}) {
        for (auto& a : lv) a.store(kNegInfDb);
    }
};

Mixer::Mixer(MixerConfig cfg)
    : count_(cfg.channelNames.size()),
      names_(std::move(cfg.channelNames)),
      sampleRate_(cfg.sampleRate),
      maxBlock_(std::max<std::size_t>(16, cfg.maxBlockFrames)),
      rampSamples_(std::max<std::size_t>(1, static_cast<std::size_t>(cfg.rampMs * cfg.sampleRate / 1000.0f))),
      limiter_(cfg.sampleRate, cfg.limiter),
      programPeak_(PeakMeterConfig{cfg.sampleRate, 1.5, 20.0}),
      programRms_(cfg.sampleRate, 0.3),
      programLoud_(cfg.sampleRate) {
    chStorage_.reserve(count_);
    for (std::size_t i = 0; i < count_; ++i) chStorage_.push_back(std::make_unique<Channel>(sampleRate_));
    scratch_.assign(maxBlock_ * 2, 0.0f);
    zeros_.assign(maxBlock_ * 2, 0.0f);
    for (auto& a : prog_) a.store(kNegInfDb);
    prog_[10].store(0.0f);
}

Mixer::~Mixer() = default;

// ---------------------------------------------------------------- control
void Mixer::setGainDb(std::size_t i, float db) noexcept {
    if (i >= count_ || std::isnan(db)) return;
    db = std::min(db, 24.0f);
    chStorage_[i]->gainDb.store(db);
    chStorage_[i]->gainLin.store(dbToLinear(db));
}
void Mixer::setFaderDb(std::size_t i, float db) noexcept {
    if (i >= count_ || std::isnan(db)) return;
    db = std::min(db, kFaderMaxDb);
    chStorage_[i]->faderDb.store(db);
    chStorage_[i]->faderLin.store(dbToLinear(db));
}
void Mixer::setFaderPosition(std::size_t i, float pos) noexcept {
    if (std::isnan(pos)) return;
    setFaderDb(i, faderPositionToDb(pos));
}
void Mixer::setMute(std::size_t i, bool on) noexcept { if (i < count_) chStorage_[i]->mute.store(on); }
void Mixer::setSolo(std::size_t i, bool on) noexcept { if (i < count_) chStorage_[i]->solo.store(on); }
void Mixer::setPan(std::size_t i, float pan) noexcept {
    if (i >= count_ || std::isnan(pan)) return;
    pan = std::clamp(pan, -1.0f, 1.0f);
    float l, r;
    panGains(pan, l, r);
    chStorage_[i]->pan.store(pan);
    chStorage_[i]->panL.store(l);
    chStorage_[i]->panR.store(r);
}
void Mixer::setMasterFaderDb(float db) noexcept {
    if (std::isnan(db)) return;
    db = std::min(db, kFaderMaxDb);
    masterDb_.store(db);
    masterGainLin_.store(dbToLinear(db));
}
float Mixer::gainDb(std::size_t i) const noexcept { return i < count_ ? chStorage_[i]->gainDb.load() : 0.0f; }
float Mixer::faderDb(std::size_t i) const noexcept { return i < count_ ? chStorage_[i]->faderDb.load() : 0.0f; }
bool Mixer::muted(std::size_t i) const noexcept { return i < count_ && chStorage_[i]->mute.load(); }
bool Mixer::soloed(std::size_t i) const noexcept { return i < count_ && chStorage_[i]->solo.load(); }
float Mixer::pan(std::size_t i) const noexcept { return i < count_ ? chStorage_[i]->pan.load() : 0.0f; }
void Mixer::attachSource(std::size_t i, AudioSource* s) noexcept { if (i < count_) chStorage_[i]->src.store(s); }

ChannelLevels Mixer::channelLevels(std::size_t i) const noexcept {
    ChannelLevels l{{kNegInfDb, kNegInfDb}, {kNegInfDb, kNegInfDb}};
    if (i < count_) {
        const auto& c = *chStorage_[i];
        l.peakDb[0] = c.lv[0].load(std::memory_order_relaxed);
        l.peakDb[1] = c.lv[1].load(std::memory_order_relaxed);
        l.holdDb[0] = c.lv[2].load(std::memory_order_relaxed);
        l.holdDb[1] = c.lv[3].load(std::memory_order_relaxed);
    }
    return l;
}

ProgramLevels Mixer::programLevels() const noexcept {
    auto g = [&](int i) { return prog_[i].load(std::memory_order_relaxed); };
    return ProgramLevels{{g(0), g(1)}, {g(2), g(3)}, {g(4), g(5)}, {g(6), g(7)}, g(8), g(9), g(10)};
}

// ------------------------------------------------------------------ audio
void Mixer::mixChannel(Channel& c, const float* in, float* out, std::size_t n, bool audible) noexcept {
    if (!in) in = zeros_.data();
    const float trim = c.gainLin.load(std::memory_order_relaxed);

    // Pre-fader input meter (after trim).
    float pl = 0, pr = 0;
    for (std::size_t i = 0; i < n; ++i) {
        pl = std::max(pl, std::fabs(in[2 * i]));
        pr = std::max(pr, std::fabs(in[2 * i + 1]));
    }
    c.meter.update(pl * trim, pr * trim, n);
    c.lv[0].store(c.meter.levelDb(0), std::memory_order_relaxed);
    c.lv[1].store(c.meter.levelDb(1), std::memory_order_relaxed);
    c.lv[2].store(c.meter.holdDb(0), std::memory_order_relaxed);
    c.lv[3].store(c.meter.holdDb(1), std::memory_order_relaxed);

    // Target gains for this block.
    const float total = audible ? trim * c.faderLin.load(std::memory_order_relaxed) : 0.0f;
    const float tL = total * c.panL.load(std::memory_order_relaxed);
    const float tR = total * c.panR.load(std::memory_order_relaxed);
    if (!c.primed) {
        c.curL = c.tgtL = tL;
        c.curR = c.tgtR = tR;
        c.remaining = 0;
        c.primed = true;
    } else if (tL != c.tgtL || tR != c.tgtR) {
        c.tgtL = tL;
        c.tgtR = tR;
        c.remaining = rampSamples_;
        c.stepL = (tL - c.curL) / static_cast<float>(rampSamples_);
        c.stepR = (tR - c.curR) / static_cast<float>(rampSamples_);
    }

    if (c.remaining == 0 && c.curL == 0.0f && c.curR == 0.0f) return;  // silent and settled

    std::size_t i = 0;
    while (i < n) {
        if (c.remaining > 0) {
            const std::size_t seg = std::min(c.remaining, n - i);
            for (std::size_t j = 0; j < seg; ++j, ++i) {
                c.curL += c.stepL;
                c.curR += c.stepR;
                out[2 * i] += in[2 * i] * c.curL;
                out[2 * i + 1] += in[2 * i + 1] * c.curR;
            }
            c.remaining -= seg;
            if (c.remaining == 0) { c.curL = c.tgtL; c.curR = c.tgtR; }
        } else {
            const float gl = c.curL, gr = c.curR;
            for (; i < n; ++i) {
                out[2 * i] += in[2 * i] * gl;
                out[2 * i + 1] += in[2 * i + 1] * gr;
            }
        }
    }
}

template <class GetInput>
void Mixer::run(GetInput&& get, float* out, std::size_t frames) noexcept {
    if (resetMeters_.exchange(false, std::memory_order_relaxed)) {
        programTp_.reset();
        programPeak_.reset();
        for (std::size_t i = 0; i < count_; ++i) chStorage_[i]->meter.reset();
    }
    for (std::size_t done = 0; done < frames;) {
        const std::size_t n = std::min(maxBlock_, frames - done);
        float* o = out + 2 * done;
        std::memset(o, 0, n * 2 * sizeof(float));

        bool anySolo = false;
        for (std::size_t i = 0; i < count_; ++i) anySolo = anySolo || chStorage_[i]->solo.load(std::memory_order_relaxed);
        for (std::size_t i = 0; i < count_; ++i) {
            Channel& c = *chStorage_[i];
            const bool audible = !c.mute.load(std::memory_order_relaxed) &&
                                 (!anySolo || c.solo.load(std::memory_order_relaxed));
            mixChannel(c, get(i, done, n), o, n, audible);
        }

        // Master fader (ramped).
        const float mt = masterGainLin_.load(std::memory_order_relaxed);
        if (mt != masterTarget_) {
            masterTarget_ = mt;
            masterRemaining_ = rampSamples_;
            masterStep_ = (mt - masterCur_) / static_cast<float>(rampSamples_);
        }
        if (masterRemaining_ == 0 && masterCur_ == 1.0f) {
            // unity, nothing to do
        } else {
            for (std::size_t i = 0; i < n; ++i) {
                if (masterRemaining_ > 0) {
                    masterCur_ += masterStep_;
                    if (--masterRemaining_ == 0) masterCur_ = masterTarget_;
                }
                o[2 * i] *= masterCur_;
                o[2 * i + 1] *= masterCur_;
            }
        }

        limiter_.process(o, n);
        programPeak_.process(o, n);
        programRms_.process(o, n);
        programTp_.process(o, n);
        programLoud_.process(o, n);
        done += n;
    }
    publishProgram();
}

void Mixer::publishProgram() noexcept {
    auto s = [&](int i, float v) { prog_[i].store(v, std::memory_order_relaxed); };
    s(0, programPeak_.levelDb(0)); s(1, programPeak_.levelDb(1));
    s(2, programPeak_.holdDb(0));  s(3, programPeak_.holdDb(1));
    s(4, programRms_.rmsDb(0));    s(5, programRms_.rmsDb(1));
    s(6, programTp_.maxDb(0));     s(7, programTp_.maxDb(1));
    s(8, programLoud_.momentaryLufs());
    s(9, programLoud_.shortTermLufs());
    s(10, limiter_.lastGainReductionDb());
}

void Mixer::process(const float* const* inputs, float* programOut, std::size_t frames) noexcept {
    run([&](std::size_t ch, std::size_t offset, std::size_t) -> const float* {
            return inputs && inputs[ch] ? inputs[ch] + 2 * offset : nullptr;
        },
        programOut, frames);
}

void Mixer::process(float* programOut, std::size_t frames) noexcept {
    run([&](std::size_t ch, std::size_t, std::size_t n) -> const float* {
            AudioSource* s = chStorage_[ch]->src.load(std::memory_order_relaxed);
            if (!s) return nullptr;
            s->pull(scratch_.data(), n);
            return scratch_.data();
        },
        programOut, frames);
}

}  // namespace hb::audio
