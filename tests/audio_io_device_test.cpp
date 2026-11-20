#include <gtest/gtest.h>

#include <chrono>
#include <set>
#include <stdexcept>
#include <thread>

#include "audio_io/device_enumerator.hpp"
#include "audio_io/streams.hpp"
#include "audio_io_test_util.hpp"
#include "core/json.hpp"

using namespace hb::audio_io;
using namespace hb::audio_io::test;

namespace {
StreamOptions manual(const std::shared_ptr<FakeWorld>& w, FakeClock& c) {
    StreamOptions o;
    o.opener = fakeOpener(w);
    o.clockNs = c.fn();
    o.supervisor = false;
    o.noSignalMs = 1000;
    return o;
}

template <class Pred>
bool waitFor(Pred p, int ms = 3000) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        if (p()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return p();
}
}  // namespace

// ------------------------------------------------------------------ enumerator
TEST(AudioIoEnumerator, NullBackendListsStableIds) {
    DeviceEnumerator e(Backend::Null);
    ASSERT_TRUE(e.refresh()) << e.lastError();
    const auto caps = e.devices(DeviceKind::Capture);
    const auto plays = e.devices(DeviceKind::Playback);
    ASSERT_FALSE(caps.empty());
    ASSERT_FALSE(plays.empty());
    EXPECT_EQ(caps[0].id.rfind("cap:null:", 0), 0u) << caps[0].id;
    EXPECT_EQ(plays[0].id.rfind("play:null:", 0), 0u) << plays[0].id;
    EXPECT_FALSE(caps[0].name.empty());
    EXPECT_GT(caps[0].sampleRate, 0);
    EXPECT_GT(caps[0].channels, 0);
    EXPECT_TRUE(e.devices(DeviceKind::Loopback).empty());  // loopback exists only on WASAPI
    std::set<std::string> ids1;
    for (const auto& d : e.devices()) ids1.insert(d.id);
    ASSERT_TRUE(e.refresh());
    std::set<std::string> ids2;
    for (const auto& d : e.devices()) ids2.insert(d.id);
    EXPECT_EQ(ids1, ids2);  // stable across refreshes
    EXPECT_EQ(ids1.size(), e.devices().size());
    EXPECT_TRUE(e.find(caps[0].id).has_value());
    EXPECT_FALSE(e.find("nada").has_value());
    hb::core::json::Value v;
    ASSERT_TRUE(hb::core::json::parse(e.toJson(), v));
    EXPECT_EQ(v.get("devices")->a.size(), e.devices().size());
}

TEST(AudioIoEnumerator, FailuresAreTolerated) {
    int mode = 0;
    DeviceEnumerator e(Backend::Auto, [&](Backend, std::string& err) -> std::vector<DeviceInfo> {
        if (mode == 0) return {{"cap:x:1", "Mic", DeviceKind::Capture, true, 2, 48000}};
        if (mode == 1) {
            err = "Nenhum sistema de áudio.";
            return {};
        }
        throw std::runtime_error("boom");
    });
    EXPECT_TRUE(e.refresh());
    ASSERT_EQ(e.devices().size(), 1u);
    mode = 1;
    EXPECT_FALSE(e.refresh());
    EXPECT_EQ(e.lastError(), "Nenhum sistema de áudio.");
    EXPECT_EQ(e.devices().size(), 1u);  // last good list kept
    mode = 2;
    EXPECT_FALSE(e.refresh());
    EXPECT_NE(e.lastError().find("boom"), std::string::npos);
    hb::core::json::Value v;
    EXPECT_TRUE(hb::core::json::parse(e.toJson(), v));
}

TEST(AudioIoEnumerator, DefaultBackendNeverCrashesWithoutHardware) {
    DeviceEnumerator e(Backend::Auto);  // CI machines have no sound system: must just report it
    const bool ok = e.refresh();
    if (!ok) EXPECT_FALSE(e.lastError().empty());
    SUCCEED();
}

// ------------------------------------------------------------------ capture state machine
TEST(AudioIoCapture, ConnectingOnlineNoSignalAndBack) {
    auto w = std::make_shared<FakeWorld>();
    FakeClock clk;
    CaptureStream s("cap:fake:1", DeviceKind::Capture, manual(w, clk));
    EXPECT_EQ(s.status().state, StreamState::Idle);
    s.start();
    s.tick();
    EXPECT_EQ(s.status().state, StreamState::Connecting);
    EXPECT_EQ(s.status().sampleRate, 48000);
    auto dev = w->current();
    ASSERT_TRUE(dev);
    std::vector<float> chunk(480 * 2, 0.1f);
    dev->cb.capture(chunk.data(), 480);
    clk.advanceMs(10);
    s.tick();
    EXPECT_EQ(s.status().state, StreamState::Online);
    EXPECT_EQ(s.status().message, "Online.");
    EXPECT_EQ(s.status().frames, 480u);
    clk.advanceMs(1500);  // no callbacks for > noSignalMs
    s.tick();
    EXPECT_EQ(s.status().state, StreamState::NoSignal);
    EXPECT_NE(s.status().message.find("Sem sinal"), std::string::npos);
    dev->cb.capture(chunk.data(), 480);
    s.tick();
    EXPECT_EQ(s.status().state, StreamState::Online);
    auto ses = s.session();
    ASSERT_TRUE(ses);
    EXPECT_EQ(ses->ring.size(), 960u * 2u);
    EXPECT_EQ(ses->sampleRate, 48000);
}

TEST(AudioIoCapture, OpenedButSilentDeviceBecomesNoSignal) {
    auto w = std::make_shared<FakeWorld>();
    FakeClock clk;
    CaptureStream s("cap:fake:1", DeviceKind::Loopback, manual(w, clk));
    s.start();
    s.tick();
    EXPECT_EQ(s.status().state, StreamState::Connecting);
    clk.advanceMs(1200);
    s.tick();
    EXPECT_EQ(s.status().state, StreamState::NoSignal);  // e.g. WASAPI loopback while nothing plays
    EXPECT_EQ(s.kind(), DeviceKind::Loopback);
}

TEST(AudioIoCapture, ErrorBackoffThenRecovers) {
    auto w = std::make_shared<FakeWorld>();
    w->failOpen = true;
    FakeClock clk;
    CaptureStream s("cap:fake:1", DeviceKind::Capture, manual(w, clk));
    s.start();
    s.tick();
    auto st = s.status();
    EXPECT_EQ(st.state, StreamState::Error);
    EXPECT_NE(st.message.find("Nova tentativa em 0,3 s"), std::string::npos) << st.message;  // 250 ms
    EXPECT_NE(st.message.find("indisponível"), std::string::npos);
    EXPECT_EQ(w->opens, 0);
    clk.advanceMs(100);
    s.tick();  // too early: no new attempt (message unchanged)
    EXPECT_EQ(s.status().message, st.message);
    clk.advanceMs(200);
    s.tick();  // attempt 2 fails -> backoff doubles to 500 ms
    EXPECT_NE(s.status().message.find("0,5 s"), std::string::npos) << s.status().message;
    for (int i = 0; i < 6; ++i) {
        clk.advanceMs(5000);
        s.tick();
    }
    EXPECT_NE(s.status().message.find("4,0 s"), std::string::npos) << s.status().message;  // capped
    w->failOpen = false;
    clk.advanceMs(5000);
    s.tick();
    EXPECT_EQ(s.status().state, StreamState::Connecting);
    EXPECT_EQ(w->opens, 1);
}

TEST(AudioIoCapture, ReopensByItselfWhenDeviceVanishes) {
    auto w = std::make_shared<FakeWorld>();
    FakeClock clk;
    CaptureStream s("cap:fake:1", DeviceKind::Capture, manual(w, clk));
    s.start();
    s.tick();
    auto d1 = w->current();
    std::vector<float> chunk(480 * 2, 0.1f);
    d1->cb.capture(chunk.data(), 480);
    s.tick();
    ASSERT_EQ(s.status().state, StreamState::Online);
    const auto firstSession = s.session();

    d1->alive = false;
    s.tick();
    EXPECT_EQ(s.status().state, StreamState::Error);
    EXPECT_NE(s.status().message.find("desconectado"), std::string::npos);
    EXPECT_EQ(s.status().sampleRate, 0);
    w->failOpen = true;  // still unplugged: attempts fail
    clk.advanceMs(300);
    s.tick();
    EXPECT_EQ(w->opens, 1);
    w->failOpen = false;
    clk.advanceMs(1000);
    s.tick();
    ASSERT_EQ(w->opens, 2);
    auto d2 = w->current();
    EXPECT_NE(d1.get(), d2.get());
    d2->cb.capture(chunk.data(), 480);
    s.tick();
    EXPECT_EQ(s.status().state, StreamState::Online);
    EXPECT_EQ(s.status().opens, 2u);
    EXPECT_NE(s.session(), firstSession);  // fresh ring: nothing stale leaks
    EXPECT_EQ(s.session()->generation, firstSession->generation + 1);
}

TEST(AudioIoCapture, StopAndStartAreIdempotentAndNeverThrow) {
    auto w = std::make_shared<FakeWorld>();
    FakeClock clk;
    CaptureStream s("cap:fake:1", DeviceKind::Capture, manual(w, clk));
    s.tick();  // not started: no-op
    s.stop();
    s.start();
    s.start();
    s.tick();
    s.stop();
    s.stop();
    EXPECT_EQ(s.status().state, StreamState::Idle);
    EXPECT_EQ(w->opens, 1);
    s.tick();
    EXPECT_EQ(w->opens, 1);
}

TEST(AudioIoCapture, ThrowingOpenerIsContained) {
    FakeClock clk;
    StreamOptions o;
    o.opener = [](const OpenRequest&, std::string&) -> std::unique_ptr<RawDevice> { throw std::runtime_error("driver"); };
    o.clockNs = clk.fn();
    o.supervisor = false;
    CaptureStream s("x", DeviceKind::Capture, o);
    s.start();
    s.tick();
    EXPECT_EQ(s.status().state, StreamState::Error);
}

TEST(AudioIoCapture, MalformedIdsFailGracefullyOnTheNullBackend) {
    StreamOptions o;
    o.backend = Backend::Null;
    o.supervisor = false;
    FakeClock clk;
    o.clockNs = clk.fn();
    CaptureStream s("lixo-sem-formato", DeviceKind::Capture, o);
    s.start();
    s.tick();
    EXPECT_EQ(s.status().state, StreamState::Error);
    EXPECT_NE(s.status().message.find("inválido"), std::string::npos) << s.status().message;
}

// ------------------------------------------------------------------ real miniaudio, null backend (no hardware)
TEST(AudioIoNullBackend, CaptureOpensAndDeliversAudioAtNativeRate) {
    DeviceEnumerator e(Backend::Null);
    ASSERT_TRUE(e.refresh());
    const auto caps = e.devices(DeviceKind::Capture);
    ASSERT_FALSE(caps.empty());
    StreamOptions o;
    o.backend = Backend::Null;
    CaptureStream s(caps[0].id, DeviceKind::Capture, o);
    s.start();
    ASSERT_TRUE(waitFor([&] { return s.status().state == StreamState::Online; })) << s.status().message;
    EXPECT_GT(s.status().sampleRate, 0);
    auto ses = s.session();
    ASSERT_TRUE(ses);
    EXPECT_TRUE(waitFor([&] { return s.status().frames > 480; }));
    EXPECT_GT(ses->ring.size(), 0u);
    s.stop();
    EXPECT_EQ(s.status().state, StreamState::Idle);
}

TEST(AudioIoNullBackend, DefaultDeviceAndPlaybackWork) {
    StreamOptions o;
    o.backend = Backend::Null;
    CaptureStream c("", DeviceKind::Capture, o);  // "" = system default
    c.start();
    ASSERT_TRUE(waitFor([&] { return c.status().state == StreamState::Online; })) << c.status().message;
    PlaybackStream p("", o, 20);
    p.start();
    ASSERT_TRUE(waitFor([&] { return p.status().state == StreamState::Online; })) << p.status().message;
    std::vector<float> blk(480 * 2, 0.1f);
    for (int i = 0; i < 20; ++i) {
        p.write(blk.data(), 480);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_GT(p.status().frames, 0u);
}

// ------------------------------------------------------------------ playback / monitoring
TEST(AudioIoPlayback, ResamplesToDeviceRateAndTracksDrift) {
    auto w = std::make_shared<FakeWorld>();
    w->rate = 44100;
    FakeClock clk;
    PlaybackStream p("play:fake:1", manual(w, clk), 40);
    p.start();
    p.tick();
    auto dev = w->current();
    ASSERT_TRUE(dev && dev->started);
    // Producer (engine) at 48 kHz exactly; device consumes at 44.1 kHz * (1 + 150 ppm).
    const double devPerTick = 441.0 * (1 + 150e-6);
    double carry = 0;
    std::vector<float> in(480 * 2, 0.25f), out;
    for (int t = 0; t < 8000; ++t) {  // 80 s
        p.write(in.data(), 480);
        carry += devPerTick;
        const int n = static_cast<int>(carry);
        carry -= n;
        out.assign(static_cast<std::size_t>(n) * 2, 7.0f);
        dev->cb.playback(out.data(), static_cast<std::size_t>(n));
        if (t > 3000) {
            for (float v : out) ASSERT_NEAR(v, 0.25f, 2e-3f) << "t=" << t;  // DC stays DC: no gaps, no clicks
        }
    }
    const auto st = p.status();
    EXPECT_EQ(st.underruns, 0u);
    EXPECT_EQ(st.overruns, 0u);
    EXPECT_NEAR(p.driftPpm(), -150.0, 40.0);  // device slower than nominal... converges near the true offset
}

TEST(AudioIoPlayback, DropsSilentlyWhileDeviceIsClosed) {
    auto w = std::make_shared<FakeWorld>();
    w->failOpen = true;
    FakeClock clk;
    PlaybackStream p("play:fake:1", manual(w, clk));
    p.start();
    p.tick();
    std::vector<float> in(480 * 2, 0.25f);
    for (int i = 0; i < 100; ++i) p.write(in.data(), 480);
    EXPECT_EQ(p.status().overruns, 0u);
    EXPECT_EQ(p.status().state, StreamState::Error);
}
