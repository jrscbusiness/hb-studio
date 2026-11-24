#include <gtest/gtest.h>

#include <cmath>
#include <vector>

#include "captions/segmenter.hpp"

using namespace hb::captions;

namespace {
std::vector<float> tone(int ms, float amp = 0.2f) {
    std::vector<float> v(static_cast<std::size_t>(ms) * 16);
    for (std::size_t i = 0; i < v.size(); ++i) v[i] = amp * std::sin(2.0f * 3.14159265f * 220.0f * static_cast<float>(i) / 16000.0f);
    return v;
}
std::vector<float> silence(int ms, float noise = 0.0005f) {
    std::vector<float> v(static_cast<std::size_t>(ms) * 16);
    for (std::size_t i = 0; i < v.size(); ++i) v[i] = (i % 2 ? noise : -noise);
    return v;
}
void append(std::vector<float>& a, const std::vector<float>& b) { a.insert(a.end(), b.begin(), b.end()); }
std::vector<SegmentEvent> run(Segmenter& s, const std::vector<float>& x, std::size_t chunk = 320) {
    std::vector<SegmentEvent> all;
    for (std::size_t i = 0; i < x.size(); i += chunk) {
        auto e = s.feed(x.data() + i, std::min(chunk, x.size() - i));
        for (auto& ev : e) all.push_back(std::move(ev));
    }
    return all;
}
}  // namespace

TEST(CaptionsSegmenter, SilenceProducesNothing) {
    Segmenter s;
    EXPECT_TRUE(run(s, silence(5000)).empty());
    EXPECT_FALSE(s.inSpeech());
}

TEST(CaptionsSegmenter, OneUtteranceGivesOneFinalWithPreRollAndTrim) {
    SegmenterConfig c;
    c.partialIntervalMs = 0;
    Segmenter s(c);
    std::vector<float> x = silence(1000);
    append(x, tone(1000));
    append(x, silence(1500));
    const auto ev = run(s, x);
    ASSERT_EQ(ev.size(), 1u);
    EXPECT_EQ(ev[0].kind, SegmentEvent::Kind::Final);
    const double durMs = static_cast<double>(ev[0].audio.size()) / 16.0;
    // 1000 ms speech + ~240 ms pre-roll + <=200 ms trailing silence kept
    EXPECT_GE(durMs, 1000.0);
    EXPECT_LE(durMs, 1000 + 240 + 200 + 40);
    EXPECT_NEAR(static_cast<double>(ev[0].startMs), 1000 - 240, 40);
    EXPECT_GT(ev[0].endMs, ev[0].startMs);
    EXPECT_FALSE(s.inSpeech());
}

TEST(CaptionsSegmenter, PartialsEveryIntervalThenFinal) {
    SegmenterConfig c;
    c.partialIntervalMs = 1000;
    Segmenter s(c);
    std::vector<float> x = silence(500);
    append(x, tone(3500));
    append(x, silence(1200));
    const auto ev = run(s, x);
    int partials = 0, finals = 0;
    std::size_t lastAudio = 0;
    for (const auto& e : ev) {
        if (e.kind == SegmentEvent::Kind::Partial) {
            ++partials;
            EXPECT_GT(e.audio.size(), lastAudio);   // grows: each partial holds the whole utterance so far
            lastAudio = e.audio.size();
        } else {
            ++finals;   // (a final is trimmed of trailing silence, so it may be slightly shorter than the last partial)
        }
        EXPECT_EQ(e.id, 1u);
    }
    EXPECT_GE(partials, 2);
    EXPECT_LE(partials, 4);
    EXPECT_EQ(finals, 1);
    EXPECT_EQ(ev.back().kind, SegmentEvent::Kind::Final);
}

TEST(CaptionsSegmenter, TwoUtterancesSeparatedByPause) {
    SegmenterConfig c;
    c.partialIntervalMs = 0;
    Segmenter s(c);
    std::vector<float> x = tone(800);
    append(x, silence(1500));
    append(x, tone(800));
    append(x, silence(1500));
    const auto ev = run(s, x);
    ASSERT_EQ(ev.size(), 2u);
    EXPECT_EQ(ev[0].id, 1u);
    EXPECT_EQ(ev[1].id, 2u);
    EXPECT_LT(ev[0].endMs, ev[1].startMs);
}

TEST(CaptionsSegmenter, ShortPauseDoesNotSplit) {
    SegmenterConfig c;
    c.partialIntervalMs = 0;
    Segmenter s(c);
    std::vector<float> x = tone(800);
    append(x, silence(300));   // < hangover
    append(x, tone(800));
    append(x, silence(1500));
    EXPECT_EQ(run(s, x).size(), 1u);
}

TEST(CaptionsSegmenter, ShortBlipIsIgnored) {
    Segmenter s;
    std::vector<float> x = silence(500);
    append(x, tone(60));   // < minSpeechMs
    append(x, silence(2000));
    EXPECT_TRUE(run(s, x).empty());
}

TEST(CaptionsSegmenter, ContinuousSpeechIsCutAtMaxLength) {
    SegmenterConfig c;
    c.partialIntervalMs = 0;
    Segmenter s(c);
    // speech-like: 900 ms bursts separated by 100 ms dips (shorter than the hangover), 30 s in all
    std::vector<float> talk;
    for (int i = 0; i < 30; ++i) { append(talk, tone(900)); append(talk, silence(100)); }
    const auto ev = run(s, talk);
    std::size_t finals = 0;
    for (const auto& e : ev) {
        if (e.kind != SegmentEvent::Kind::Final) continue;
        ++finals;
        EXPECT_LE(e.audio.size(), static_cast<std::size_t>(c.maxUtteranceMs) * 16 + 640);
    }
    EXPECT_GE(finals, 2u);
}

TEST(CaptionsSegmenter, SoftCutAtPauseAfterSoftMax) {
    SegmenterConfig c;
    c.partialIntervalMs = 0;
    c.hangoverMs = 2000;   // pause alone would not close it
    Segmenter s(c);
    std::vector<float> x = tone(8500);
    append(x, silence(300));   // >= softCutSilenceMs while past softMaxMs
    append(x, tone(500));
    const auto ev = run(s, x);
    ASSERT_GE(ev.size(), 1u);
    EXPECT_EQ(ev[0].kind, SegmentEvent::Kind::Final);
    EXPECT_LT(static_cast<double>(ev[0].audio.size()) / 16.0, 9500.0);
}

TEST(CaptionsSegmenter, NoiseFloorAdaptsToNoisyRoom) {
    SegmenterConfig c;
    c.partialIntervalMs = 0;
    Segmenter s(c);
    // steady noise at about -31 dBFS: above the fixed floor but must stop counting as speech once the
    // minimum-statistics floor catches up (window noiseWindowMs)
    std::vector<float> noise(16000 * 14);
    for (std::size_t i = 0; i < noise.size(); ++i) noise[i] = 0.05f * ((i * 7919) % 200 / 100.0f - 1.0f);
    run(s, noise);
    EXPECT_FALSE(s.inSpeech());
    EXPECT_GT(s.noiseFloorDb(), -40.0);
    // real speech well above that noise is still detected
    std::vector<float> speech = tone(800, 0.3f);
    append(speech, silence(1500, 0.05f));
    const auto ev2 = run(s, speech);
    bool any = false;
    for (const auto& e : ev2) any = any || e.kind == SegmentEvent::Kind::Final;
    EXPECT_TRUE(any);
}

TEST(CaptionsSegmenter, FlushClosesOpenUtteranceAndOddChunkSizesWork) {
    SegmenterConfig c;
    c.partialIntervalMs = 0;
    Segmenter s(c);
    const auto ev = run(s, tone(900), 173);   // chunk size not a multiple of the frame
    EXPECT_TRUE(ev.empty());
    EXPECT_TRUE(s.inSpeech());
    const auto f = s.flush();
    ASSERT_EQ(f.size(), 1u);
    EXPECT_EQ(f[0].kind, SegmentEvent::Kind::Final);
    EXPECT_FALSE(s.inSpeech());
    EXPECT_TRUE(s.feed(nullptr, 10).empty());
}
