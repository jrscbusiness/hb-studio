#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <mutex>
#include <thread>
#include <vector>

#include "captions/service.hpp"
#include "captions/session.hpp"

using namespace hb::captions;

namespace {
std::vector<float> tone(int ms, float amp = 0.2f) {
    std::vector<float> v(static_cast<std::size_t>(ms) * 16);
    for (std::size_t i = 0; i < v.size(); ++i) v[i] = amp * std::sin(2.0f * 3.14159265f * 220.0f * static_cast<float>(i) / 16000.0f);
    return v;
}
std::vector<float> quiet(int ms) { return std::vector<float>(static_cast<std::size_t>(ms) * 16, 0.0f); }
void append(std::vector<float>& a, const std::vector<float>& b) { a.insert(a.end(), b.begin(), b.end()); }

struct Captured {
    std::mutex m;
    std::vector<hb::core::Fields> events;
    void add(const hb::core::Event& e) { std::lock_guard l(m); events.push_back(e.fields); }
    std::vector<hb::core::Fields> get() { std::lock_guard l(m); return events; }
};
}  // namespace

TEST(CaptionsSession, PublishesPartialsThenFinalWithFrozenContract) {
    hb::core::EventBus bus;
    Captured cap;
    bus.subscribe("caption.text", [&](const hb::core::Event& e) { cap.add(e); });
    // partial hypotheses grow; final is the complete sentence
    FakeRecognizer rec([](const RecognizeRequest& r, int call) -> std::string {
        if (r.partial) return call == 0 ? "bom dia" : "bom dia a todos";
        return "bom dia a todos vocês";
    });
    SessionConfig cfg;
    cfg.language = "pt";
    cfg.segmenter.partialIntervalMs = 1000;
    std::int64_t clock = 0;
    CaptionSession s(rec, &bus, cfg, [&] { return clock += 50; });   // each decode "takes" 50 ms
    std::vector<float> x = quiet(500);
    append(x, tone(2600));
    append(x, quiet(1500));
    for (std::size_t i = 0; i < x.size(); i += 480) s.feed(x.data() + i, std::min<std::size_t>(480, x.size() - i), 100);
    const auto ev = cap.get();
    ASSERT_GE(ev.size(), 3u);
    for (const auto& f : ev) {
        EXPECT_EQ(f.size(), 3u);   // exactly text, final, lang
        EXPECT_TRUE(f.at("final") == "0" || f.at("final") == "1");
        EXPECT_EQ(f.at("lang"), "pt");
    }
    EXPECT_EQ(ev.front().at("final"), "0");
    EXPECT_EQ(ev.front().at("text"), "Bom dia");
    EXPECT_EQ(ev.back().at("final"), "1");
    EXPECT_EQ(ev.back().at("text"), "Bom dia a todos vocês.");
    const auto st = s.stats();
    EXPECT_EQ(st.utterances, 1u);
    EXPECT_EQ(st.finalsPublished, 1u);
    EXPECT_EQ(st.lastDecodeMs, 50);
    EXPECT_EQ(st.lastLatencyMs, 150);   // decode + backlog
    ASSERT_EQ(s.srt().size(), 1u);
    EXPECT_EQ(s.srt().cues()[0].text, "Bom dia a todos vocês.");
}

TEST(CaptionsSession, LongFinalIsPaginatedIntoSeveralCaptions) {
    hb::core::EventBus bus;
    Captured cap;
    bus.subscribe("caption.text", [&](const hb::core::Event& e) { cap.add(e); });
    FakeRecognizer rec([](const RecognizeRequest&, int) { return std::string("uma frase bem longa que não cabe em duas linhas curtas de legenda na tela do espectador"); }, false);
    SessionConfig cfg;
    cfg.normalize.maxCharsPerLine = 20;
    cfg.normalize.maxLines = 2;
    CaptionSession s(rec, &bus, cfg, nullptr);
    std::vector<float> x = tone(1200);
    append(x, quiet(1500));
    s.feed(x.data(), x.size());
    const auto ev = cap.get();
    ASSERT_GE(ev.size(), 2u);
    for (const auto& f : ev) {
        EXPECT_EQ(f.at("final"), "1");
        EXPECT_LE(f.at("text").size(), 2u * 20u + 1u + 4u);   // two lines (+ newline, + UTF-8 slack)
    }
    EXPECT_EQ(s.srt().size(), ev.size());
    EXPECT_EQ(rec.calls(), 1);   // engine without partials: only the final was decoded
}

TEST(CaptionsSession, RecognizerErrorsAreCountedNotPublishedNorFatal) {
    hb::core::EventBus bus;
    Captured cap;
    bus.subscribe("*", [&](const hb::core::Event& e) { cap.add(e); });
    FakeRecognizer rec([](const RecognizeRequest&, int) { return std::string("x"); });
    rec.failAll = true;
    CaptionSession s(rec, &bus, SessionConfig{}, nullptr);
    std::vector<float> x = tone(1000);
    append(x, quiet(1500));
    s.feed(x.data(), x.size());
    EXPECT_TRUE(cap.get().empty());
    EXPECT_GE(s.stats().recognizerErrors, 1u);
    EXPECT_FALSE(s.stats().lastError.empty());
}

TEST(CaptionsSession, ThrowingRecognizerIsContained) {
    FakeRecognizer rec([](const RecognizeRequest&, int) -> std::string { throw std::runtime_error("boom"); });
    CaptionSession s(rec, nullptr, SessionConfig{}, nullptr);
    std::vector<float> x = tone(1000);
    append(x, quiet(1500));
    EXPECT_NO_THROW(s.feed(x.data(), x.size()));
    EXPECT_GE(s.stats().recognizerErrors, 1u);
}

TEST(CaptionsSession, PartialsSkippedWhenBehind) {
    FakeRecognizer rec([](const RecognizeRequest&, int) { return std::string("a"); });
    SessionConfig cfg;
    cfg.segmenter.partialIntervalMs = 500;
    CaptionSession s(rec, nullptr, cfg, nullptr);
    std::vector<float> x = tone(2500);
    s.feed(x.data(), x.size(), 5000);   // 5 s backlog
    EXPECT_EQ(rec.calls(), 0);
    EXPECT_GE(s.stats().partialsSkipped, 1u);
}

TEST(CaptionsSession, FinishFlushesOpenUtterance) {
    hb::core::EventBus bus;
    Captured cap;
    bus.subscribe("caption.text", [&](const hb::core::Event& e) { cap.add(e); });
    FakeRecognizer rec([](const RecognizeRequest&, int) { return std::string("fim"); }, false);
    CaptionSession s(rec, &bus, SessionConfig{}, nullptr);
    const auto x = tone(1000);
    s.feed(x.data(), x.size());
    EXPECT_TRUE(cap.get().empty());
    s.finish();
    ASSERT_EQ(cap.get().size(), 1u);
    EXPECT_EQ(cap.get()[0].at("text"), "Fim.");
}

TEST(CaptionsService, EndToEndThroughQueueAndWorkerThread) {
    hb::core::EventBus bus;
    Captured cap;
    bus.subscribe("caption.text", [&](const hb::core::Event& e) { cap.add(e); });
    auto rec = std::make_unique<FakeRecognizer>([](const RecognizeRequest&, int) { return std::string("teste de legenda"); }, false);
    CaptionService svc(std::move(rec), &bus, SessionConfig{});
    svc.start();
    // wait for the worker to start listening
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (svc.status().state != ServiceStatus::State::Listening && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    ASSERT_EQ(svc.status().state, ServiceStatus::State::Listening);
    // 48 kHz stereo: 1 s of tone then 1.5 s of silence, in 10 ms engine blocks
    std::vector<float> blk(480 * 2);
    long n = 0;
    auto feed = [&](int ms, float amp) {
        for (int b = 0; b < ms / 10; ++b) {
            for (int i = 0; i < 480; ++i, ++n) blk[i * 2] = blk[i * 2 + 1] = amp * std::sin(2.0f * 3.14159265f * 220.0f * static_cast<float>(n) / 48000.0f);
            svc.pushProgramAudio(blk.data(), 480);
        }
    };
    feed(1000, 0.2f);
    feed(1500, 0.0f);
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (cap.get().empty() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    svc.stop();
    ASSERT_FALSE(cap.get().empty());
    EXPECT_EQ(cap.get().back().at("text"), "Teste de legenda.");
    EXPECT_EQ(svc.status().state, ServiceStatus::State::Stopped);
    EXPECT_EQ(svc.srtCueCount(), 1u);
    EXPECT_NE(svc.srtText().find("Teste de legenda."), std::string::npos);
    svc.pushProgramAudio(blk.data(), 480);   // after stop: ignored, no crash
}

TEST(CaptionsService, LoadFailureGoesToErrorState) {
    auto rec = std::make_unique<FakeRecognizer>(nullptr);
    rec->loadError = "modelo ausente";
    CaptionService svc(std::move(rec), nullptr, SessionConfig{});
    svc.start();
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (svc.status().state == ServiceStatus::State::Loading && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_EQ(svc.status().state, ServiceStatus::State::Error);
    EXPECT_EQ(svc.status().error, "modelo ausente");
    svc.stop();
    EXPECT_EQ(svc.status().state, ServiceStatus::State::Error);   // stays visible after stop
}
