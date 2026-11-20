#pragma once
// Helpers shared by the audio_io tests: fake devices, synthetic sources, a fake effect, a test rig.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "audio/audio_common.hpp"
#include "audio/effect.hpp"
#include "audio_io/audio_engine.hpp"
#include "audio_io/streams.hpp"

namespace hb::audio_io::test {

// ---------------------------------------------------------------- fake devices
struct FakeDeviceState {
    int rate = 48000;
    int channels = 2;
    std::atomic<bool> alive{true};
    RawCallbacks cb;  // set by start()
    std::atomic<bool> started{false};
};

// Controls what the fake opener does; shared by the test and the opener closure.
struct FakeWorld {
    std::mutex m;
    bool failOpen = false;
    int rate = 48000;
    int opens = 0;
    std::vector<std::string> openedIds;
    std::shared_ptr<FakeDeviceState> last;  // most recently opened device

    std::shared_ptr<FakeDeviceState> current() {
        std::lock_guard l(m);
        return last;
    }
};

class FakeDevice final : public RawDevice {
public:
    explicit FakeDevice(std::shared_ptr<FakeDeviceState> s) : st_(std::move(s)) {}
    int sampleRate() const override { return st_->rate; }
    int nativeChannels() const override { return st_->channels; }
    bool start(RawCallbacks cb, std::string&) override {
        st_->cb = std::move(cb);
        st_->started = true;
        return true;
    }
    bool alive() const override { return st_->alive.load(); }

private:
    std::shared_ptr<FakeDeviceState> st_;
};

inline DeviceOpener fakeOpener(std::shared_ptr<FakeWorld> w) {
    return [w](const OpenRequest& req, std::string& err) -> std::unique_ptr<RawDevice> {
        std::lock_guard l(w->m);
        if (w->failOpen) {
            err = "dispositivo simulado indisponível.";
            return nullptr;
        }
        auto st = std::make_shared<FakeDeviceState>();
        st->rate = w->rate;
        w->last = st;
        ++w->opens;
        w->openedIds.push_back(req.deviceId);
        return std::make_unique<FakeDevice>(st);
    };
}

// Monotonic fake clock in nanoseconds.
struct FakeClock {
    std::atomic<std::int64_t> ns{1'000'000'000};
    std::function<std::int64_t()> fn() {
        return [this] { return ns.load(); };
    }
    void advanceMs(std::int64_t ms) { ns += ms * 1'000'000; }
};

// ---------------------------------------------------------------- sources
// Constant stereo value.
class ConstSource final : public hb::audio::AudioSource {
public:
    ConstSource(float l, float r) : l_(l), r_(r) {}
    std::size_t pull(float* out, std::size_t frames) noexcept override {
        for (std::size_t i = 0; i < frames; ++i) {
            out[2 * i] = l_;
            out[2 * i + 1] = r_;
        }
        return frames;
    }

private:
    float l_, r_;
};

// Sine (phase continuous), same on both channels.
class SineSource final : public hb::audio::AudioSource {
public:
    SineSource(double hz, double peak, int rate = 48000) : hz_(hz), peak_(peak), rate_(rate) {}
    int sampleRate() const noexcept override { return rate_; }
    std::size_t pull(float* out, std::size_t frames) noexcept override {
        for (std::size_t i = 0; i < frames; ++i) {
            const float v = static_cast<float>(peak_ * std::sin(2.0 * hb::audio::kPi * hz_ * static_cast<double>(n_++) / rate_));
            out[2 * i] = out[2 * i + 1] = v;
        }
        return frames;
    }

private:
    double hz_, peak_;
    int rate_;
    std::uint64_t n_ = 0;
};

// Silence with a single full-scale sample at absolute frame `at`.
class ImpulseSource final : public hb::audio::AudioSource {
public:
    explicit ImpulseSource(std::uint64_t at) : at_(at) {}
    std::size_t pull(float* out, std::size_t frames) noexcept override {
        for (std::size_t i = 0; i < frames; ++i, ++n_) {
            const float v = n_ == at_ ? 0.5f : 0.0f;
            out[2 * i] = out[2 * i + 1] = v;
        }
        return frames;
    }

private:
    std::uint64_t at_, n_ = 0;
};

// ---------------------------------------------------------------- fake effects
// "test.gain": multiplies by `gain`; reports gain reduction `gr` (set by the test).
class GainEffect final : public hb::audio::Effect {
public:
    GainEffect() {
        params_.push_back({"gain", "Ganho", "x", 0.0f, 4.0f, 1.0f, false, {}});
        params_.push_back({"mode", "Modo", "", 0.0f, 2.0f, 0.0f, false, {"A", "B", "C"}});
    }
    const char* typeId() const override { return "test.gain"; }
    const std::vector<hb::audio::EffectParamInfo>& params() const override { return params_; }
    void setParam(const std::string& id, float v) override {
        if (id == "gain") gain_.store(std::clamp(v, 0.0f, 4.0f));
        else if (id == "mode") mode_.store(std::clamp(v, 0.0f, 2.0f));
    }
    float getParam(const std::string& id) const override {
        if (id == "gain") return gain_.load();
        if (id == "mode") return mode_.load();
        return 0.0f;
    }
    void prepare(int, int) override {}
    void process(float* io, std::size_t frames) override {
        const float g = gain_.load(std::memory_order_relaxed);
        for (std::size_t i = 0; i < frames * 2; ++i) io[i] *= g;
        processed_.fetch_add(frames, std::memory_order_relaxed);
    }
    void reset() override {}
    float gainReductionDb() const override { return gain_.load() < 1.0f ? -20.0f * std::log10(std::max(gain_.load(), 1e-6f)) : 0.0f; }
    std::atomic<std::uint64_t> processed_{0};

private:
    std::vector<hb::audio::EffectParamInfo> params_;
    std::atomic<float> gain_{1.0f}, mode_{0.0f};
};

// "test.invert": polarity flip, no parameters.
class InvertEffect final : public hb::audio::Effect {
public:
    const char* typeId() const override { return "test.invert"; }
    const std::vector<hb::audio::EffectParamInfo>& params() const override { return params_; }
    void setParam(const std::string&, float) override {}
    float getParam(const std::string&) const override { return 0.0f; }
    void prepare(int, int) override {}
    void process(float* io, std::size_t frames) override {
        for (std::size_t i = 0; i < frames * 2; ++i) io[i] = -io[i];
    }
    void reset() override {}

private:
    std::vector<hb::audio::EffectParamInfo> params_;
};

inline void registerTestEffects() {
    auto& r = hb::audio::EffectRegistry::instance();
    r.add({"test.gain", "Ganho de teste", "Utilitários", "test"}, [] { return std::make_unique<GainEffect>(); });
    r.add({"test.invert", "Inversor de teste", "Utilitários", "test"}, [] { return std::make_unique<InvertEffect>(); });
}

// ---------------------------------------------------------------- engine rig (manual clock, no threads)
struct Rig {
    std::shared_ptr<FakeWorld> world = std::make_shared<FakeWorld>();
    FakeClock clock;
    std::unique_ptr<AudioEngine> engine;
    std::vector<float> program;  // everything delivered to the consumer
    std::size_t blocksSeen = 0;
    bool badBlock = false;

    explicit Rig(int strips = 8, bool limiterOn = false) {
        registerTestEffects();
        EngineOptions o;
        o.strips = strips;
        o.opener = fakeOpener(world);
        o.clockNs = clock.fn();
        o.streamSupervisors = false;
        o.backend = Backend::Null;
        o.lister = [](Backend, std::string&) { return std::vector<DeviceInfo>{}; };
        engine = std::make_unique<AudioEngine>(o);
        engine->setMasterParam("limiter", limiterOn ? 1 : 0);
        engine->addProgramConsumer([this](const float* p, std::size_t n) {
            if (n != kBlockFrames) badBlock = true;
            program.insert(program.end(), p, p + n * 2);
            ++blocksSeen;
        });
    }
    // Runs n blocks of 10 ms.
    void run(int n) {
        for (int i = 0; i < n; ++i) {
            engine->processBlock();
            clock.advanceMs(10);
        }
    }
    float lastL() const { return program[program.size() - 2]; }
    float lastR() const { return program[program.size() - 1]; }
};

inline double dbOf(double lin) { return 20.0 * std::log10(std::max(lin, 1e-12)); }

}  // namespace hb::audio_io::test
