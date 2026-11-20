#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>

#include "audio_io_test_util.hpp"
#include "core/json.hpp"

using namespace hb::audio_io;
using namespace hb::audio_io::test;

namespace {
constexpr float kSqrtHalf = 0.70710678f;
constexpr float kTol = 2e-5f;

void ext(Rig& r, int strip, std::shared_ptr<hb::audio::AudioSource> src) {
    const std::string id = "src" + std::to_string(strip);
    ASSERT_TRUE(r.engine->registerExternalSource(id, std::move(src)));
    ASSERT_TRUE(r.engine->setStripInput(strip, "external", id));
}
}  // namespace

// ------------------------------------------------------------------ mixing in exact numbers
TEST(AudioEngineMix, SumOfTwoStripsPannedHardLeft) {
    Rig r;
    ext(r, 0, std::make_shared<ConstSource>(0.2f, 0.2f));
    ext(r, 1, std::make_shared<ConstSource>(0.1f, 0.3f));
    r.engine->setStripParam(0, "pan", -1);
    r.engine->setStripParam(1, "pan", -1);
    r.run(6);
    EXPECT_NEAR(r.lastL(), 0.3f, kTol);
    EXPECT_NEAR(r.lastR(), 0.0f, kTol);
    EXPECT_FALSE(r.badBlock);
}

TEST(AudioEngineMix, ConstantPowerPan) {
    Rig r;
    ext(r, 0, std::make_shared<ConstSource>(0.5f, 0.5f));
    r.run(6);  // centre
    EXPECT_NEAR(r.lastL(), 0.5f * kSqrtHalf, kTol);
    EXPECT_NEAR(r.lastR(), 0.5f * kSqrtHalf, kTol);
    r.engine->setStripParam(0, "pan", 1);
    r.run(6);
    EXPECT_NEAR(r.lastL(), 0.0f, kTol);
    EXPECT_NEAR(r.lastR(), 0.5f, kTol);
}

TEST(AudioEngineMix, FaderTrimAndMaster) {
    Rig r;
    ext(r, 0, std::make_shared<ConstSource>(0.4f, 0.4f));
    r.engine->setStripParam(0, "pan", -1);
    r.engine->setStripParam(0, "faderDb", -6.0206);
    r.run(6);
    EXPECT_NEAR(r.lastL(), 0.2f, kTol);
    r.engine->setStripParam(0, "trimDb", 6.0206);  // back to 0.4
    r.run(6);
    EXPECT_NEAR(r.lastL(), 0.4f, kTol);
    r.engine->setMasterParam("faderDb", -6.0206);
    r.run(6);
    EXPECT_NEAR(r.lastL(), 0.2f, kTol);
    r.engine->setStripParam(0, "faderDb", -200);  // -inf
    r.run(6);
    EXPECT_EQ(r.lastL(), 0.0f);
}

TEST(AudioEngineMix, MuteAndSoloInPlace) {
    Rig r;
    ext(r, 0, std::make_shared<ConstSource>(0.2f, 0.2f));
    ext(r, 1, std::make_shared<ConstSource>(0.1f, 0.1f));
    r.engine->setStripParam(0, "pan", -1);
    r.engine->setStripParam(1, "pan", -1);
    r.engine->setStripParam(0, "mute", 1);
    r.run(6);
    EXPECT_NEAR(r.lastL(), 0.1f, kTol);
    r.engine->setStripParam(0, "mute", 0);
    r.engine->setStripParam(1, "solo", 1);  // solo-in-place: only strip 1 reaches the bus
    r.run(6);
    EXPECT_NEAR(r.lastL(), 0.1f, kTol);
    r.engine->setStripParam(1, "mute", 1);  // mute beats solo
    r.run(6);
    EXPECT_NEAR(r.lastL(), 0.0f, kTol);
}

TEST(AudioEngineMix, PhaseInvertAndMono) {
    Rig r;
    ext(r, 0, std::make_shared<ConstSource>(0.4f, 0.2f));
    r.engine->setStripParam(0, "pan", -1);  // only L survives pan: isolates the channel under test
    r.run(6);
    EXPECT_NEAR(r.lastL(), 0.4f, kTol);
    r.engine->setStripParam(0, "phase", 1);
    r.run(6);
    EXPECT_NEAR(r.lastL(), -0.4f, kTol);
    r.engine->setStripParam(0, "phase", 0);
    r.engine->setStripParam(0, "mono", 1);  // (0.4 + 0.2) / 2
    r.run(6);
    EXPECT_NEAR(r.lastL(), 0.3f, kTol);
    r.engine->setStripParam(0, "pan", 1);
    r.run(6);
    EXPECT_NEAR(r.lastR(), 0.3f, kTol);
}

TEST(AudioEngineMix, SendToProgramOff) {
    Rig r;
    ext(r, 0, std::make_shared<ConstSource>(0.4f, 0.4f));
    r.engine->setStripParam(0, "toProgram", 0);
    r.run(6);
    EXPECT_EQ(r.lastL(), 0.0f);
}

namespace {
std::size_t firstAbove(const std::vector<float>& v, float thr) {
    for (std::size_t i = 0; i + 1 < v.size(); i += 2)
        if (std::fabs(v[i]) > thr) return i / 2;
    return SIZE_MAX;
}
}  // namespace

TEST(AudioEngineMix, DelayIsExactInSamples) {
    constexpr std::uint64_t at = 6000;  // well after the 20 ms delay crossfade
    Rig a, b;
    for (Rig* r : {&a, &b}) {
        ext(*r, 0, std::make_shared<ImpulseSource>(at));
        r->engine->setStripParam(0, "pan", -1);
    }
    b.engine->setStripParam(0, "delayMs", 5.0);  // 240 samples
    a.run(30);
    b.run(30);
    const std::size_t pa = firstAbove(a.program, 0.1f), pb = firstAbove(b.program, 0.1f);
    ASSERT_NE(pa, SIZE_MAX);
    ASSERT_NE(pb, SIZE_MAX);
    EXPECT_EQ(pb - pa, 240u);
    EXPECT_GE(pa, at);
}

// ------------------------------------------------------------------ validation
TEST(AudioEngineCommands, ValidationNeverCrashes) {
    Rig r;
    EXPECT_FALSE(r.engine->setStripParam(-1, "mute", 1));
    EXPECT_FALSE(r.engine->setStripParam(8, "mute", 1));
    EXPECT_FALSE(r.engine->setStripParam(0, "mute", std::nan("")));
    EXPECT_FALSE(r.engine->setStripParam(0, "bogus", 1));
    EXPECT_FALSE(r.engine->lastError().empty());
    EXPECT_TRUE(r.engine->setStripParam(0, "pan", 55));  // clamped
    EXPECT_TRUE(r.engine->setStripParam(0, "trimDb", 1e30));
    EXPECT_TRUE(r.engine->setStripParam(0, "faderDb", 1e30));
    EXPECT_TRUE(r.engine->setStripParam(0, "delayMs", -4));
    EXPECT_TRUE(r.engine->setStripParam(0, "delayMs", INFINITY));
    auto s = r.engine->snapshot();
    EXPECT_EQ(s.strips[0].pan, 1.0f);
    EXPECT_EQ(s.strips[0].trimDb, 24.0f);
    EXPECT_EQ(s.strips[0].faderDb, 10.0f);
    EXPECT_EQ(s.strips[0].delayMs, 2000.0f);
    EXPECT_FALSE(r.engine->setStripQuality(0, "ultra"));
    EXPECT_TRUE(r.engine->setStripQuality(0, "high"));
    EXPECT_FALSE(r.engine->setStripInput(0, "banana"));
    EXPECT_FALSE(r.engine->setStripInput(0, "device", ""));
    EXPECT_FALSE(r.engine->setStripInput(0, "device", "play:null:n0"));
    EXPECT_FALSE(r.engine->setStripInput(0, "device", std::string(5000, 'x')));
    EXPECT_FALSE(r.engine->setMasterParam("x", 1));
    EXPECT_FALSE(r.engine->setMasterParam("faderDb", std::nan("")));
    EXPECT_TRUE(r.engine->setMasterParam("ceilingDb", -100));
    EXPECT_EQ(r.engine->snapshot().master.ceilingDb, -12.0f);
    EXPECT_TRUE(r.engine->setMonitor("level", 7));
    EXPECT_EQ(r.engine->snapshot().monitor.level, 1.0f);
    EXPECT_FALSE(r.engine->setMonitor("deviceId", "cap:null:n0"));
    EXPECT_FALSE(r.engine->setStripParam(0, "name", "   "));
    EXPECT_TRUE(r.engine->setStripParam(0, "name", std::string("Narrador\x01\x02 ") + std::string(100, 'z')));
    EXPECT_LE(r.engine->snapshot().strips[0].name.size(), 32u);
    EXPECT_EQ(r.engine->snapshot().strips[1].name, "MIC 2");
    EXPECT_EQ(r.engine->snapshot().strips[2].name, "LINHA");
    EXPECT_EQ(r.engine->snapshot().strips[3].name, "SISTEMA");
    EXPECT_EQ(r.engine->stripCount(), 8);
}

// ------------------------------------------------------------------ effects
TEST(AudioEngineEffects, ChainProcessesInOrderBypassMoveRemove) {
    Rig r;
    ext(r, 0, std::make_shared<ConstSource>(0.4f, 0.4f));
    r.engine->setStripParam(0, "pan", -1);
    ASSERT_TRUE(r.engine->addEffect(0, "test.gain"));
    ASSERT_TRUE(r.engine->addEffect(0, "test.invert", 0));  // inserted before
    auto s = r.engine->snapshot();
    ASSERT_EQ(s.strips[0].effects.size(), 2u);
    EXPECT_EQ(s.strips[0].effects[0].type, "test.invert");
    EXPECT_EQ(s.strips[0].effects[1].type, "test.gain");
    EXPECT_EQ(s.strips[0].effects[1].name, "Ganho de teste");
    ASSERT_EQ(s.strips[0].effects[1].info.size(), 2u);
    EXPECT_EQ(s.strips[0].effects[1].values[0], 1.0f);

    ASSERT_TRUE(r.engine->setEffectParam(0, 1, "gain", 0.5));
    r.run(6);
    EXPECT_NEAR(r.lastL(), -0.2f, kTol);
    ASSERT_TRUE(r.engine->setEffectBypass(0, 0, true));  // bypass the inverter
    r.run(6);
    EXPECT_NEAR(r.lastL(), 0.2f, kTol);
    ASSERT_TRUE(r.engine->moveEffect(0, 1, 0));
    EXPECT_EQ(r.engine->snapshot().strips[0].effects[0].type, "test.gain");
    ASSERT_TRUE(r.engine->removeEffect(0, 0));  // gain gone
    r.run(6);
    EXPECT_NEAR(r.lastL(), 0.4f, kTol);  // inverter still bypassed
    EXPECT_EQ(r.engine->snapshot().strips[0].effects.size(), 1u);
    ASSERT_TRUE(r.engine->setEffectBypass(0, 0, false));
    r.run(6);
    EXPECT_NEAR(r.lastL(), -0.4f, kTol);
}

TEST(AudioEngineEffects, ValidationAndLimits) {
    Rig r;
    EXPECT_FALSE(r.engine->addEffect(0, "nao.existe"));
    EXPECT_FALSE(r.engine->addEffect(9, "test.gain"));
    EXPECT_FALSE(r.engine->removeEffect(0, 0));
    EXPECT_FALSE(r.engine->moveEffect(0, 0, 1));
    ASSERT_TRUE(r.engine->addEffect(0, "test.gain"));
    EXPECT_FALSE(r.engine->setEffectParam(0, 0, "gain", std::nan("")));
    EXPECT_FALSE(r.engine->setEffectParam(0, 0, "nope", 1));
    EXPECT_FALSE(r.engine->setEffectParam(0, 3, "gain", 1));
    EXPECT_FALSE(r.engine->setEffectParam(0, -1, "gain", 1));
    EXPECT_FALSE(r.engine->setEffectBypass(0, 5, true));
    EXPECT_TRUE(r.engine->setEffectParam(0, 0, "gain", 1e9));  // clamped to max
    EXPECT_EQ(r.engine->snapshot().strips[0].effects[0].values[0], 4.0f);
    EXPECT_TRUE(r.engine->setEffectParam(0, 0, "mode", 1.6));  // choice index rounds
    EXPECT_EQ(r.engine->snapshot().strips[0].effects[0].values[1], 2.0f);
    for (int i = 1; i < kMaxEffects; ++i) ASSERT_TRUE(r.engine->addEffect(0, "test.invert"));
    EXPECT_FALSE(r.engine->addEffect(0, "test.invert"));  // 9th
    EXPECT_EQ(r.engine->snapshot().strips[0].effects.size(), 8u);
}

TEST(AudioEngineEffects, GainReductionIsMetered) {
    Rig r;
    ext(r, 0, std::make_shared<ConstSource>(0.4f, 0.4f));
    ASSERT_TRUE(r.engine->addEffect(0, "test.gain"));
    ASSERT_TRUE(r.engine->setEffectParam(0, 0, "gain", 0.5));
    r.run(3);
    const auto s = r.engine->snapshot();
    EXPECT_NEAR(s.strips[0].gainReductionDb, 6.02f, 0.05f);
    EXPECT_NEAR(s.strips[0].effects[0].gainReductionDb, 6.02f, 0.05f);
    const std::string lv = r.engine->snapshotJson(false);
    EXPECT_NE(lv.find("\"gr\":[6"), std::string::npos) << lv;
}

TEST(AudioEngineEffects, ChainSwapWhileProcessingInTwoThreads) {
    Rig r;
    ext(r, 0, std::make_shared<SineSource>(1000, 0.5));
    ext(r, 1, std::make_shared<SineSource>(440, 0.5));
    std::atomic<bool> stop{false};
    std::atomic<int> blocks{0};
    std::thread audio([&] {
        while (!stop.load()) {
            r.engine->processBlock();
            ++blocks;
        }
    });
    auto hammer = [&](int strip) {
        int n = 0;
        while (n < 400) {
            r.engine->addEffect(strip, n % 2 ? "test.gain" : "test.invert");
            r.engine->setEffectParam(strip, 0, "gain", (n % 7) * 0.5);
            r.engine->moveEffect(strip, 0, 1);
            r.engine->setEffectBypass(strip, 0, n % 3 == 0);
            if (n % 3 == 2) r.engine->removeEffect(strip, 0);
            r.engine->setStripParam(strip, "faderDb", -(n % 20));
            r.engine->setStripQuality(strip, n % 2 ? "high" : "linear");
            (void)r.engine->snapshotJson(n % 5 == 0);
            ++n;
        }
    };
    std::thread t1(hammer, 0), t2(hammer, 1);
    t1.join();
    t2.join();
    stop = true;
    audio.join();
    EXPECT_GT(blocks.load(), 10);
    for (float v : r.program) ASSERT_TRUE(std::isfinite(v));
    EXPECT_LE(r.engine->snapshot().strips[0].effects.size(), 8u);
}

// ------------------------------------------------------------------ meters
TEST(AudioEngineMeters, StripAndProgramLevelsOfAKnownSine) {
    Rig r;
    const double peak = std::pow(10.0, -23.0 / 20.0);  // -23 dBFS peak per channel
    ext(r, 0, std::make_shared<SineSource>(1000, peak));
    r.run(500);  // 5 s: fills the 3 s short-term window
    auto s = r.engine->snapshot();
    EXPECT_NEAR(s.strips[0].peakDb[0], -23.0f, 0.1f);
    EXPECT_NEAR(s.strips[0].rmsDb[0], -26.01f, 0.15f);
    EXPECT_NEAR(s.strips[0].lufsShort, -22.99f, 0.15f);  // same reference as the hb_audio_tests (EBU 3341 style)
    EXPECT_LT(s.strips[1].peakDb[0], -100.0f);          // empty strip: silence
    // Program: pan centre costs 3.01 dB on each channel
    EXPECT_NEAR(s.master.levels.peakDb[0], -23.0f - 3.01f, 0.15f);
    EXPECT_NEAR(s.master.levels.shortTermLufs, -22.99f - 3.01f, 0.2f);
    EXPECT_NEAR(s.master.levels.truePeakDb[0], -26.0f, 0.3f);
}

TEST(AudioEngineMeters, LevelsJsonIsCompactAndFinite) {
    Rig r;
    const std::string quiet = r.engine->snapshotJson(false);
    EXPECT_EQ(quiet.find("inf"), std::string::npos);
    EXPECT_EQ(quiet.find("nan"), std::string::npos);
    hb::core::json::Value v;
    ASSERT_TRUE(hb::core::json::parse(quiet, v)) << quiet;
    EXPECT_EQ(v.get("t")->s, "audiolv");
    ASSERT_EQ(v.get("strips")->a.size(), 8u);
    EXPECT_EQ(v.get("strips")->a[0].get("pk")->n, -120);
    ASSERT_NE(v.get("master"), nullptr);
    for (const char* k : {"pk", "hold", "rms", "lm", "ls", "tp", "gr"}) EXPECT_NE(v.get("master")->get(k), nullptr) << k;
    EXPECT_NE(v.get("xr"), nullptr);
}

// ------------------------------------------------------------------ Program consumers
TEST(AudioEngineProgram, ThirtySimulatedSecondsContinuousBlocks) {
    Rig r;
    ext(r, 0, std::make_shared<SineSource>(1000, 0.5));
    r.engine->setMasterParam("limiter", 1);
    r.run(3000);
    EXPECT_EQ(r.blocksSeen, 3000u);
    EXPECT_FALSE(r.badBlock);
    ASSERT_EQ(r.program.size(), 3000u * 480u * 2u);
    float maxStep = 0;
    for (std::size_t i = 4000; i + 2 < r.program.size(); i += 2) maxStep = std::max(maxStep, std::fabs(r.program[i + 2] - r.program[i]));
    EXPECT_LT(maxStep, 0.06f);  // phase continuous sine: no clicks, no dropped blocks
    const auto st = r.engine->stats();
    EXPECT_EQ(st.blocks, 3000u);
    EXPECT_EQ(st.xruns, 0u);
    EXPECT_EQ(st.consumerErrors, 0u);
}

TEST(AudioEngineProgram, ThrowingConsumerIsIsolated) {
    Rig r;
    int calls = 0;
    r.engine->addProgramConsumer([](const float*, std::size_t) { throw 1; });
    r.engine->addProgramConsumer([&](const float*, std::size_t) { ++calls; });
    r.run(10);
    EXPECT_EQ(calls, 10);
    EXPECT_EQ(r.engine->stats().consumerErrors, 10u);
}

TEST(AudioEngineProgram, RemoveConsumerIsSynchronous) {
    Rig r;
    std::atomic<int> calls{0};
    std::atomic<bool> inside{false}, stop{false};
    auto id = r.engine->addProgramConsumer([&](const float*, std::size_t) {
        inside = true;
        std::this_thread::sleep_for(std::chrono::microseconds(200));
        ++calls;
        inside = false;
    });
    std::thread audio([&] {
        while (!stop) r.engine->processBlock();
    });
    while (calls < 20) std::this_thread::yield();
    r.engine->removeProgramConsumer(id);
    EXPECT_FALSE(inside.load());
    const int frozen = calls.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    EXPECT_EQ(calls.load(), frozen);
    stop = true;
    audio.join();
}

TEST(AudioEngineProgram, ConsumerMayRemoveItself) {
    Rig r;
    int calls = 0;
    AudioEngine::ConsumerId id = 0;
    id = r.engine->addProgramConsumer([&](const float*, std::size_t) {
        ++calls;
        r.engine->removeProgramConsumer(id);
    });
    r.run(5);
    EXPECT_EQ(calls, 1);
}

TEST(AudioEngineProgram, RealTimeThreadKeepsTheRhythm) {
    EngineOptions o;
    o.backend = Backend::Null;
    o.lister = [](Backend, std::string&) { return std::vector<DeviceInfo>{}; };
    AudioEngine e(o);
    std::atomic<int> blocks{0};
    e.addProgramConsumer([&](const float*, std::size_t n) {
        if (n == kBlockFrames) ++blocks;
    });
    e.start();
    EXPECT_TRUE(e.running());
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    e.stop();
    EXPECT_FALSE(e.running());
    EXPECT_GE(blocks.load(), 44);
    EXPECT_LE(blocks.load(), 51);
    e.start();  // restartable
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    e.stop();
}

// ------------------------------------------------------------------ devices through the engine
TEST(AudioEngineCapture, DeviceAtNativeRateReachesTheProgram) {
    Rig r;
    r.world->rate = 44100;
    ASSERT_TRUE(r.engine->setStripInput(0, "device", "cap:null:n0"));
    r.engine->setStripParam(0, "pan", -1);
    r.engine->runMaintenance();  // opens the (fake) device and builds the 44.1 -> 48 kHz input
    auto dev = r.world->current();
    ASSERT_TRUE(dev);
    ASSERT_TRUE(dev->started);
    std::vector<float> chunk(441 * 2, 0.25f);
    for (int i = 0; i < 100; ++i) {
        dev->cb.capture(chunk.data(), 441);
        r.run(1);
        if (i % 5 == 0) r.engine->runMaintenance();
    }
    EXPECT_NEAR(r.lastL(), 0.25f, 1e-3f);
    const auto s = r.engine->snapshot();
    EXPECT_EQ(s.strips[0].inputState, "online");
    EXPECT_EQ(r.engine->stats().underruns, 0u);
}

TEST(AudioEngineCapture, DeviceVanishesAndComesBackWithAnotherRate) {
    Rig r;
    ASSERT_TRUE(r.engine->setStripInput(0, "device", "cap:null:n0"));
    r.engine->setStripParam(0, "pan", -1);
    r.engine->runMaintenance();
    auto d1 = r.world->current();
    std::vector<float> chunk(480 * 2, 0.5f);
    for (int i = 0; i < 30; ++i) {
        d1->cb.capture(chunk.data(), 480);
        r.run(1);
        r.engine->runMaintenance();
    }
    EXPECT_NEAR(r.lastL(), 0.5f, 1e-3f);

    d1->alive = false;  // unplugged
    r.engine->runMaintenance();
    auto s = r.engine->snapshot();
    EXPECT_EQ(s.strips[0].inputState, "error");
    EXPECT_NE(s.strips[0].inputMessage.find("desconectado"), std::string::npos) << s.strips[0].inputMessage;
    r.run(5);  // engine keeps running (silence)
    EXPECT_NEAR(r.lastL(), 0.0f, 1e-3f);

    r.world->rate = 32000;  // comes back as a different device/rate
    r.clock.advanceMs(300);
    r.engine->runMaintenance();
    auto d2 = r.world->current();
    ASSERT_NE(d1.get(), d2.get());
    std::vector<float> c2(320 * 2, -0.3f);
    for (int i = 0; i < 60; ++i) {
        d2->cb.capture(c2.data(), 320);
        r.run(1);
        r.engine->runMaintenance();
    }
    EXPECT_NEAR(r.lastL(), -0.3f, 2e-3f);
    EXPECT_EQ(r.engine->snapshot().strips[0].inputState, "online");
}

namespace {
void runDrift(int rate, double ppm, double seconds) {
    Rig r;
    r.world->rate = rate;
    ASSERT_TRUE(r.engine->setStripInput(0, "device", "cap:null:n0"));
    r.engine->setStripParam(0, "pan", -1);
    r.engine->runMaintenance();
    auto dev = r.world->current();
    ASSERT_TRUE(dev);
    const double perTick = rate * 0.01 * (1.0 + ppm * 1e-6);
    double carry = 0, phase = 0;
    std::vector<float> buf;
    const int ticks = static_cast<int>(seconds * 100);
    for (int t = 0; t < ticks; ++t) {
        carry += perTick;
        const int n = static_cast<int>(carry);
        carry -= n;
        buf.resize(static_cast<std::size_t>(n) * 2);
        for (int i = 0; i < n; ++i) {
            buf[2 * i] = buf[2 * i + 1] = static_cast<float>(0.5 * std::sin(phase));
            phase += 2.0 * hb::audio::kPi * 1000.0 / rate;
        }
        dev->cb.capture(buf.data(), static_cast<std::size_t>(n));
        r.run(1);
        if (t % 5 == 0) r.engine->runMaintenance();
    }
    const auto st = r.engine->stats();
    EXPECT_EQ(st.underruns, 0u) << rate << " Hz " << ppm << " ppm";
    EXPECT_EQ(st.overruns, 0u) << rate << " Hz " << ppm << " ppm";
    float maxStep = 0;
    for (std::size_t i = 4 * 48000 * 2; i + 2 < r.program.size(); i += 2)
        maxStep = std::max(maxStep, std::fabs(r.program[i + 2] - r.program[i]));
    EXPECT_LT(maxStep, 0.07f) << "discontinuity in the compensated signal";
    EXPECT_EQ(r.engine->snapshot().strips[0].inputState, "online");
}
}  // namespace

TEST(AudioEngineDrift, Plus150ppmAt48k) { runDrift(48000, 150.0, 60); }
TEST(AudioEngineDrift, Minus150ppmAt44k1) { runDrift(44100, -150.0, 60); }

// ------------------------------------------------------------------ snapshot JSON
TEST(AudioEngineSnapshot, FullJsonHasTheAgreedSchema) {
    Rig r;
    ASSERT_TRUE(r.engine->addEffect(2, "test.gain"));
    ASSERT_TRUE(r.engine->setStripParam(0, "name", "Narração \"A\""));
    const unsigned ver = r.engine->version();
    const std::string j = r.engine->snapshotJson(true);
    hb::core::json::Value v;
    ASSERT_TRUE(hb::core::json::parse(j, v)) << j;
    EXPECT_EQ(v.get("t")->s, "audio");
    EXPECT_EQ(static_cast<unsigned>(v.get("ver")->n), ver);
    const auto& strips = v.get("strips")->a;
    ASSERT_EQ(strips.size(), 8u);
    EXPECT_EQ(strips[0].get("name")->s, "Narração \"A\"");
    for (const char* k : {"i", "name", "input", "trimDb", "faderDb", "mute", "solo", "pan", "phase", "mono", "delayMs",
                          "quality", "toProgram", "toMonitor", "fx"})
        EXPECT_NE(strips[0].get(k), nullptr) << k;
    for (const char* k : {"kind", "id", "name", "state", "msg"}) EXPECT_NE(strips[0].get("input")->get(k), nullptr) << k;
    EXPECT_EQ(strips[0].get("input")->get("kind")->s, "none");
    const auto& fx = strips[2].get("fx")->a;
    ASSERT_EQ(fx.size(), 1u);
    EXPECT_EQ(fx[0].get("type")->s, "test.gain");
    EXPECT_FALSE(fx[0].get("missing")->b);
    const auto& params = fx[0].get("params")->a;
    ASSERT_EQ(params.size(), 2u);
    for (const char* k : {"id", "label", "unit", "min", "max", "def", "log", "choices", "value"})
        EXPECT_NE(params[0].get(k), nullptr) << k;
    EXPECT_EQ(params[1].get("choices")->a.size(), 3u);
    for (const char* k : {"faderDb", "limiter", "ceilingDb"}) EXPECT_NE(v.get("master")->get(k), nullptr) << k;
    for (const char* k : {"deviceId", "level", "mute", "state", "msg"}) EXPECT_NE(v.get("monitor")->get(k), nullptr) << k;
    EXPECT_TRUE(v.get("devices")->isArr());
    bool found = false;
    for (const auto& e : v.get("effects")->a) found |= e.get("typeId")->s == "test.gain";
    EXPECT_TRUE(found);
    EXPECT_EQ(v.get("monitor")->get("state")->s, "off");
}

TEST(AudioEngineSnapshot, VersionBumpsOnConfigNotOnLevels) {
    Rig r;
    const unsigned v0 = r.engine->version();
    r.run(10);
    (void)r.engine->snapshotJson(false);
    EXPECT_EQ(r.engine->version(), v0);
    r.engine->setStripParam(0, "mute", 1);
    EXPECT_GT(r.engine->version(), v0);
    const unsigned v1 = r.engine->version();
    r.engine->setStripParam(0, "mute", std::nan(""));  // rejected: no change
    EXPECT_EQ(r.engine->version(), v1);
}

// ------------------------------------------------------------------ shutdown
TEST(AudioEngineLifecycle, CleanShutdownWithActiveThreadsAndDevices) {
    for (int round = 0; round < 3; ++round) {
        EngineOptions o;
        auto world = std::make_shared<FakeWorld>();
        o.opener = fakeOpener(world);
        o.backend = Backend::Null;
        o.lister = [](Backend, std::string&) { return std::vector<DeviceInfo>{}; };
        auto e = std::make_unique<AudioEngine>(o);
        std::atomic<int> got{0};
        e->addProgramConsumer([&](const float*, std::size_t) { ++got; });
        for (int i = 0; i < 4; ++i) e->setStripInput(i, "device", "cap:null:n" + std::to_string(i));
        e->setMonitor("deviceId", "play:null:n0");
        registerTestEffects();
        e->addEffect(0, "test.gain");
        e->start();
        std::thread poke([&] {
            for (int i = 0; i < 50; ++i) {
                e->setStripParam(i % 8, "faderDb", -i);
                (void)e->snapshotJson(i % 2 == 0);
            }
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        poke.join();
        e.reset();  // destructor while the engine, maintenance and stream supervisors are running
        EXPECT_GT(got.load(), 0);
    }
}

// ------------------------------------------------------------------ cost (informational, generous bound)
TEST(AudioEngineBench, CpuCostPerStrip) {
    for (int strips : {1, 4, 8}) {
        Rig r(8, true);
        for (int i = 0; i < strips; ++i) {
            ext(r, i, std::make_shared<SineSource>(200.0 + 100 * i, 0.3));
            r.engine->addEffect(i, "test.gain");
            r.engine->addEffect(i, "test.invert");
            r.engine->setStripParam(i, "toMonitor", 1);
        }
        r.program.clear();
        const auto t0 = std::chrono::steady_clock::now();
        const int blocks = 1000;
        for (int b = 0; b < blocks; ++b) r.engine->processBlock();
        const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / blocks;
        std::printf("[bench] %d active strips (2 effects each, limiter on, all meters): %.1f us per 10 ms block = %.3f %% of one core\n",
                    strips, us, us / 100.0);
        RecordProperty("us_per_block_" + std::to_string(strips) + "_strips", static_cast<int>(us));
        EXPECT_LT(us, 5000.0);  // 50 % of real time: only a sanity bound (sanitizer builds are much slower)
    }
}
