#include <gtest/gtest.h>

#include "streaming/stream_stats.hpp"

using namespace hb::streaming;

namespace {
struct Clock {
    std::int64_t ms = 1'000'000;
    StreamStats::NowMs fn() { return [this] { return ms; }; }
};
}  // namespace

TEST(StreamStats, EmptyHasZerosAndNoHistory) {
    Clock c;
    StreamStats s(c.fn());
    const auto snap = s.snapshot();
    EXPECT_EQ(snap.actualKbps, 0.0);
    EXPECT_TRUE(snap.history.empty());
    EXPECT_EQ(snap.droppedVideo + snap.droppedAudio, 0u);
}

TEST(StreamStats, ActualBitrateOverSlidingWindow) {
    Clock c;
    StreamStats s(c.fn(), 10);
    s.setTargetKbps(1000);
    // 125000 bytes per second = 1000 kbps, for 5 seconds
    for (int i = 0; i < 5; ++i) {
        s.onSent(true, 100000);
        s.onSent(false, 25000);
        c.ms += 1000;
    }
    auto snap = s.snapshot();
    EXPECT_NEAR(snap.actualKbps, 1000.0, 1.0);   // divided by the 5 elapsed seconds, not the 10 s window
    EXPECT_EQ(snap.targetKbps, 1000);
    EXPECT_EQ(snap.sentBytes, 625000u);
    ASSERT_EQ(snap.history.size(), 5u);
    for (int v : snap.history) EXPECT_EQ(v, 1000);

    // Stop sending for 20 s: the window slides past all the data
    c.ms += 20000;
    snap = s.snapshot();
    EXPECT_NEAR(snap.actualKbps, 0.0, 0.01);
    EXPECT_EQ(snap.sentBytes, 625000u);   // lifetime total is unaffected
}

TEST(StreamStats, WindowReflectsRecentRateNotLifetimeAverage) {
    Clock c;
    StreamStats s(c.fn(), 5);
    for (int i = 0; i < 20; ++i) { s.onSent(true, 250000); c.ms += 1000; }   // 2000 kbps for 20 s
    for (int i = 0; i < 5; ++i) { s.onSent(true, 62500); c.ms += 1000; }     // then 500 kbps for 5 s
    EXPECT_NEAR(s.snapshot().actualKbps, 500.0, 1.0);
}

TEST(StreamStats, HistoryIsCappedAndGapsAreZero) {
    Clock c;
    StreamStats s(c.fn());
    s.onSent(true, 125000);          // second 0: 1000 kbps
    c.ms += 3000;                    // seconds 1 and 2 empty
    s.onSent(true, 62500);           // second 3: 500 kbps
    c.ms += 1000;
    auto h = s.snapshot().history;
    ASSERT_EQ(h.size(), 4u);
    EXPECT_EQ(h[0], 1000); EXPECT_EQ(h[1], 0); EXPECT_EQ(h[2], 0); EXPECT_EQ(h[3], 500);
    c.ms += 100'000;
    s.onSent(true, 1);
    c.ms += 1000;
    EXPECT_LE(s.snapshot().history.size(), static_cast<std::size_t>(StreamStats::kHistorySeconds));
}

TEST(StreamStats, DropsReconnectsQueueErrorAndEncoder) {
    Clock c;
    StreamStats s(c.fn(), 10);
    s.onDropped(true, 3);
    s.onDropped(false);
    s.onReconnect();
    s.onReconnect();
    s.setQueueDepth(12, 34);
    s.setLastError("falha \"x\"");
    s.setEncoderName("nvenc");
    s.setEncoderKbps(4200);
    auto snap = s.snapshot();
    EXPECT_EQ(snap.droppedVideo, 3u);
    EXPECT_EQ(snap.droppedAudio, 1u);
    EXPECT_EQ(snap.droppedRecent, 4u);
    EXPECT_EQ(snap.reconnects, 2u);
    EXPECT_EQ(snap.queueVideo, 12);
    EXPECT_EQ(snap.queueAudio, 34);
    EXPECT_EQ(snap.encoderName, "nvenc");
    EXPECT_EQ(snap.encoderKbps, 4200);
    c.ms += 30'000;
    EXPECT_EQ(s.snapshot().droppedRecent, 0u);   // recent drops age out, totals stay
    EXPECT_EQ(s.snapshot().droppedVideo, 3u);
}

TEST(StreamStats, ResetClearsEverything) {
    Clock c;
    StreamStats s(c.fn());
    s.onSent(true, 1000);
    s.onDropped(true);
    s.onReconnect();
    s.setLastError("x");
    s.reset();
    const auto snap = s.snapshot();
    EXPECT_EQ(snap.sentBytes, 0u);
    EXPECT_EQ(snap.reconnects, 0u);
    EXPECT_TRUE(snap.lastError.empty());
}

TEST(StreamStats, JsonIsWellFormedAndEscaped) {
    Clock c;
    StreamStats s(c.fn());
    s.setTargetKbps(6000);
    s.setLastError("erro \"q\"\\ \n fim");
    s.setEncoderName("x264");
    s.onSent(true, 125000);
    c.ms += 2000;
    const std::string j = statsToJson(s.snapshot());
    EXPECT_NE(j.find("\"targetKbps\":6000"), std::string::npos);
    EXPECT_NE(j.find("\"encoder\":\"x264\""), std::string::npos);
    EXPECT_NE(j.find("\\\"q\\\""), std::string::npos);
    EXPECT_NE(j.find("\\u000a"), std::string::npos);
    EXPECT_NE(j.find("\"hist\":[1000,0]"), std::string::npos) << j;
    EXPECT_EQ(j.front(), '{');
    EXPECT_EQ(j.back(), '}');
}
