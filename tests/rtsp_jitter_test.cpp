#include <gtest/gtest.h>

#include <random>

#include "rtsp/jitter_buffer.hpp"
#include "rtsp/stream_receiver.hpp"
#include "rtsp_test_support.hpp"

using namespace hb::rtsp;
using namespace hb::rtsp::test;
using hb::core::LatencyMode;

namespace {
Bytes pkt(std::uint16_t seq, std::uint32_t ts = 0, std::uint32_t ssrc = 7, std::uint8_t pt = 96) {
    return buildRtp(pt, false, seq, ts, ssrc, Bytes{1, 2, 3, 4});
}
struct JRig {
    JitterBuffer jb;
    std::vector<OwnedRtpPacket> out;
    TestClock clk;
    explicit JRig(JitterConfig c = {}) : jb(c) {}
    void push(const Bytes& b) {
        auto v = parseRtp(b);
        ASSERT_TRUE(v.has_value());
        jb.push(*v, clk.now(), out);
    }
    void poll() { jb.poll(clk.now(), out); }
    std::vector<int> seqs() const {
        std::vector<int> s;
        for (auto& p : out) s.push_back(p.sequence);
        return s;
    }
};
JitterConfig cfg(int waitMs, std::size_t cap = 512, bool dropOldest = true) {
    JitterConfig c;
    c.maxReorderWait = Millis(waitMs);
    c.maxPackets = cap;
    c.dropOldestOnOverflow = dropOldest;
    return c;
}
}  // namespace

TEST(Jitter, InOrderReleasedImmediately) {
    JRig r(cfg(100));
    for (int i = 0; i < 5; ++i) r.push(pkt(100 + i));
    EXPECT_EQ(r.seqs(), (std::vector<int>{100, 101, 102, 103, 104}));
    EXPECT_EQ(r.jb.pending(), 0u);
    EXPECT_EQ(r.jb.stats().lost, 0u);
}

TEST(Jitter, ReordersWithinWindow) {
    JRig r(cfg(100));
    r.push(pkt(10));
    r.push(pkt(12));
    r.push(pkt(13));
    EXPECT_EQ(r.seqs(), (std::vector<int>{10}));
    EXPECT_EQ(r.jb.pending(), 2u);
    r.push(pkt(11));
    EXPECT_EQ(r.seqs(), (std::vector<int>{10, 11, 12, 13}));
    EXPECT_EQ(r.jb.stats().reordered, 1u);
    EXPECT_EQ(r.jb.stats().lost, 0u);
}

TEST(Jitter, GivesUpOnGapAfterWaitAndCountsLoss) {
    JRig r(cfg(50));
    r.push(pkt(1));
    r.push(pkt(3));
    r.clk.advance(Millis(49));
    r.poll();
    EXPECT_EQ(r.seqs(), (std::vector<int>{1}));
    r.clk.advance(Millis(2));
    r.poll();
    EXPECT_EQ(r.seqs(), (std::vector<int>{1, 3}));
    EXPECT_EQ(r.jb.stats().lost, 1u);
    EXPECT_NEAR(r.jb.stats().lossPercent, 100.0 / 3, 0.01);
    // The missing packet shows up late: counted as late, never delivered out of order.
    r.push(pkt(2));
    EXPECT_EQ(r.seqs(), (std::vector<int>{1, 3}));
    EXPECT_EQ(r.jb.stats().late, 1u);
    EXPECT_EQ(r.jb.stats().lost, 0u);  // it did arrive, per RFC 3550 accounting
}

TEST(Jitter, ZeroWaitSkipsGapsImmediately) {
    JRig r(cfg(0));
    r.push(pkt(1));
    r.push(pkt(5));
    EXPECT_EQ(r.seqs(), (std::vector<int>{1, 5}));
}

TEST(Jitter, DuplicatesDiscardedWhetherPendingOrReleased) {
    JRig r(cfg(100));
    r.push(pkt(1));
    r.push(pkt(1));  // already released
    r.push(pkt(3));
    r.push(pkt(3));  // pending duplicate
    r.push(pkt(2));
    EXPECT_EQ(r.seqs(), (std::vector<int>{1, 2, 3}));
    EXPECT_EQ(r.jb.stats().duplicates, 2u);
    EXPECT_EQ(r.jb.stats().received, 3u);
}

TEST(Jitter, SequenceWraparound) {
    JRig r(cfg(100));
    for (int s : {65534, 65535, 0, 1}) r.push(pkt(static_cast<std::uint16_t>(s)));
    EXPECT_EQ(r.seqs(), (std::vector<int>{65534, 65535, 0, 1}));
    EXPECT_EQ(r.jb.stats().extHighestSeq, 65536u + 1);
    // reorder across the wrap
    JRig w(cfg(100));
    w.push(pkt(65535));
    w.push(pkt(1));
    w.push(pkt(0));
    EXPECT_EQ(w.seqs(), (std::vector<int>{65535, 0, 1}));
    EXPECT_EQ(w.jb.stats().lost, 0u);
}

TEST(Jitter, OverflowDropOldestGivesUpOnGapEarly) {
    JRig r(cfg(10000, 4, true));
    r.push(pkt(1));
    for (int s : {3, 4, 5, 6}) r.push(pkt(static_cast<std::uint16_t>(s)));
    EXPECT_EQ(r.seqs(), (std::vector<int>{1, 3, 4, 5, 6}));  // did not wait the full 10 s
    EXPECT_EQ(r.jb.stats().lost, 1u);
}

TEST(Jitter, OverflowKeepOldestDropsNewcomers) {
    JRig r(cfg(10000, 3, false));
    r.push(pkt(1));
    for (int s : {3, 4, 5, 6, 7}) r.push(pkt(static_cast<std::uint16_t>(s)));
    EXPECT_EQ(r.jb.pending(), 3u);
    EXPECT_EQ(r.jb.stats().overflowDrops, 2u);
    r.push(pkt(2));
    EXPECT_EQ(r.seqs(), (std::vector<int>{1, 2, 3, 4, 5}));
}

TEST(Jitter, ForeignPayloadTypeAndSsrcAreIgnored) {
    auto c = cfg(100);
    c.expectedPayloadType = 96;
    JRig r(c);
    r.push(pkt(1, 0, 7, 96));
    r.push(pkt(2, 0, 7, 8));      // wrong payload type
    r.push(pkt(2, 0, 99, 96));    // other SSRC (single stray packet)
    r.push(pkt(2, 0, 7, 96));
    EXPECT_EQ(r.seqs(), (std::vector<int>{1, 2}));
    EXPECT_EQ(r.jb.stats().foreign, 2u);
}

TEST(Jitter, SsrcChangeAfterThreePacketsRestartsStream) {
    JRig r(cfg(100));
    r.push(pkt(10, 0, 1));
    r.push(pkt(11, 0, 1));
    r.push(pkt(500, 0, 2));
    r.push(pkt(501, 0, 2));
    EXPECT_EQ(r.seqs(), (std::vector<int>{10, 11}));
    r.push(pkt(502, 0, 2));
    EXPECT_EQ(r.seqs(), (std::vector<int>{10, 11, 502}));  // first two of the new source were consumed as probation
    EXPECT_EQ(r.jb.stats().resyncs, 1u);
}

TEST(Jitter, BigSequenceJumpResyncsAfterTwoConsecutivePackets) {
    JRig r(cfg(100));
    r.push(pkt(100));
    r.push(pkt(101));
    r.push(pkt(30000));  // wild jump: suspicious, dropped
    EXPECT_EQ(r.seqs(), (std::vector<int>{100, 101}));
    r.push(pkt(30001));  // continues from the jump: restart detected
    EXPECT_EQ(r.seqs(), (std::vector<int>{100, 101, 30001}));
    EXPECT_EQ(r.jb.stats().resyncs, 1u);
    EXPECT_LT(r.jb.stats().lossPercent, 1.0);  // the jump itself is not counted as 30000 lost packets
    r.push(pkt(30002));
    EXPECT_EQ(r.seqs().back(), 30002);
}

TEST(Jitter, LossStatisticsUnderRandomLossAndReorder) {
    JRig r(cfg(30));
    std::mt19937 g(9);
    std::vector<Bytes> sent;
    for (int i = 0; i < 2000; ++i) sent.push_back(pkt(static_cast<std::uint16_t>(i + 60000)));  // crosses wrap
    int dropped = 0;
    for (std::size_t i = 0; i < sent.size(); ++i) {
        if (i != 0 && i + 1 != sent.size() && g() % 20 == 0) { ++dropped; continue; }
        if (i + 1 < sent.size() && g() % 10 == 0) {  // swap with next
            r.push(sent[i + 1]);
            r.push(sent[i]);
            ++i;
        } else {
            r.push(sent[i]);
        }
        r.clk.advance(Millis(1));
        r.poll();
    }
    r.clk.advance(Millis(100));
    r.poll();
    // ordering invariant
    for (std::size_t i = 1; i < r.out.size(); ++i) {
        std::uint16_t d = static_cast<std::uint16_t>(r.out[i].sequence - r.out[i - 1].sequence);
        EXPECT_TRUE(d >= 1 && d < 0x8000);
    }
    EXPECT_NEAR(double(r.jb.stats().lost), double(dropped), 40.0);  // swapped-late packets may count as lost-then-late
    EXPECT_GT(r.jb.stats().expected, 1900u);
    EXPECT_GT(r.jb.stats().lossPercent, 2.0);
    EXPECT_LT(r.jb.stats().lossPercent, 9.0);
}

TEST(Jitter, ForModeFollowsLatencyParams) {
    auto low = JitterConfig::forMode(LatencyMode::Low, false);
    auto bal = JitterConfig::forMode(LatencyMode::Balanced, false);
    auto sta = JitterConfig::forMode(LatencyMode::Stable, false);
    EXPECT_EQ(low.maxReorderWait.count(), hb::core::latencyParams(LatencyMode::Low).networkBufferMs);
    EXPECT_EQ(bal.maxReorderWait.count(), hb::core::latencyParams(LatencyMode::Balanced).networkBufferMs);
    EXPECT_EQ(sta.maxReorderWait.count(), hb::core::latencyParams(LatencyMode::Stable).networkBufferMs);
    EXPECT_LT(low.maxReorderWait, bal.maxReorderWait);
    EXPECT_LT(bal.maxReorderWait, sta.maxReorderWait);
    EXPECT_LT(low.maxPackets, sta.maxPackets);
    EXPECT_TRUE(low.dropOldestOnOverflow);
    EXPECT_FALSE(sta.dropOldestOnOverflow);
    // TCP cannot reorder: the wait is capped.
    EXPECT_LE(JitterConfig::forMode(LatencyMode::Stable, true).maxReorderWait.count(), 20);
}

TEST(Jitter, InterarrivalJitterIsMeasured) {
    JRig steady(cfg(100));
    for (int i = 0; i < 100; ++i) {
        steady.push(pkt(static_cast<std::uint16_t>(i), static_cast<std::uint32_t>(i * 3600)));
        steady.clk.advance(Millis(40));  // exactly 3600 ticks
    }
    EXPECT_LE(steady.jb.stats().jitter, 2u);
    JRig bumpy(cfg(100));
    std::mt19937 g(3);
    for (int i = 0; i < 100; ++i) {
        bumpy.push(pkt(static_cast<std::uint16_t>(i), static_cast<std::uint32_t>(i * 3600)));
        bumpy.clk.advance(Millis(30 + g() % 20));
    }
    EXPECT_GT(bumpy.jb.stats().jitter, 100u);
}

TEST(Jitter, ReportBlockFractionLostCoversInterval) {
    JRig r(cfg(0));
    for (int i = 0; i < 10; ++i) r.push(pkt(static_cast<std::uint16_t>(i)));
    auto b1 = r.jb.reportBlock(0, 0);
    EXPECT_EQ(b1.fractionLost, 0);
    for (int i = 10; i < 20; ++i)
        if (i % 2) r.push(pkt(static_cast<std::uint16_t>(i)));  // half lost
    auto b2 = r.jb.reportBlock(0x1234, 5);
    EXPECT_NEAR(b2.fractionLost, 128, 30);  // ~50 % in 1/256 units (tail packet missing counts later)
    EXPECT_EQ(b2.cumulativeLost, 5u);  // 10,12,14,16,18
    EXPECT_EQ(b2.lastSr, 0x1234u);
    EXPECT_EQ(b2.sourceSsrc, 7u);
    EXPECT_EQ(b2.extHighestSeq, 19u);
}

TEST(Bitrate, SlidingWindow) {
    BitrateMeter m(Millis(1000));
    TestClock c;
    for (int i = 0; i < 10; ++i) {
        m.add(c.now(), 12500);  // 100 kbit each 100 ms -> 1 Mbit/s
        c.advance(Millis(100));
    }
    EXPECT_NEAR(m.bitsPerSecond(c.now()), 1.0e6, 1.2e5);
    c.advance(Millis(2000));
    EXPECT_EQ(m.bitsPerSecond(c.now()), 0.0);
}

// ---------------------------------------------------------------- AccessUnitQueue
namespace {
AccessUnit au(bool key, std::uint8_t tag) {
    AccessUnit a;
    a.keyframe = key;
    a.data = {tag};
    return a;
}
}  // namespace

TEST(AuQueue, FifoBelowCapacity) {
    AccessUnitQueue q(4, true);
    q.push(au(true, 1));
    q.push(au(false, 2));
    EXPECT_EQ(q.pop()->data[0], 1);
    EXPECT_EQ(q.pop()->data[0], 2);
    EXPECT_FALSE(q.pop());
}

TEST(AuQueue, DropOldestJumpsToNewestKeyframe) {
    AccessUnitQueue q(4, true);
    q.push(au(true, 1));
    q.push(au(false, 2));
    q.push(au(true, 3));
    q.push(au(false, 4));
    q.push(au(false, 5));  // overflow: keep from keyframe 3
    std::vector<int> got;
    while (auto a = q.pop()) got.push_back(a->data[0]);
    EXPECT_EQ(got, (std::vector<int>{3, 4, 5}));
    EXPECT_EQ(q.dropped(), 2u);
}

TEST(AuQueue, OverflowWithoutKeyframeWaitsForNextOne) {
    AccessUnitQueue q(2, true);
    q.push(au(false, 1));
    q.push(au(false, 2));
    q.push(au(false, 3));  // nothing decodable: clear, wait for keyframe
    EXPECT_EQ(q.size(), 0u);
    q.push(au(false, 4));  // ignored
    EXPECT_EQ(q.size(), 0u);
    q.push(au(true, 5));
    EXPECT_EQ(q.pop()->data[0], 5);
}

TEST(AuQueue, KeepOldestPolicyRejectsNewcomer) {
    AccessUnitQueue q(2, false);
    q.push(au(true, 1));
    q.push(au(false, 2));
    q.push(au(false, 3));  // rejected, now waiting for keyframe
    q.push(au(false, 4));
    EXPECT_EQ(q.size(), 2u);
    EXPECT_EQ(q.pop()->data[0], 1);
    EXPECT_EQ(q.pop()->data[0], 2);
    q.push(au(true, 5));
    EXPECT_EQ(q.pop()->data[0], 5);
}

// ---------------------------------------------------------------- StreamReceiver end to end
TEST(StreamReceiver, ReordersDepacketizesAndReportsStats) {
    StreamReceiverConfig rc;
    rc.codec = Codec::H264;
    rc.payloadType = 96;
    rc.jitter = cfg(50);
    rc.paramSets = {sps264(), pps264()};
    rc.receiverReportInterval = Millis(1000);
    StreamReceiver rx(rc);
    std::vector<AccessUnit> aus;
    rx.setSink([&](AccessUnit&& a) { aus.push_back(std::move(a)); });
    TestClock clk;
    Packetizer p;
    p.mtu = 100;
    auto idr = p.h264({idr264(400)}, 1000);
    // deliver with two adjacent packets swapped
    std::swap(idr[1], idr[2]);
    for (auto& b : idr) { rx.onRtp(b, clk.now()); clk.advance(Millis(2)); }
    ASSERT_EQ(aus.size(), 1u);
    EXPECT_EQ(aus[0].data, annexB({sps264(), pps264(), idr264(400)}));
    rx.onRtp(Bytes{1, 2, 3}, clk.now());  // garbage
    EXPECT_EQ(rx.snapshot(clk.now()).malformedPackets, 1u);
    EXPECT_GT(rx.snapshot(clk.now()).bitrateBps, 0.0);

    auto rr = rx.takeReceiverReport(clk.now());
    ASSERT_TRUE(rr);
    EXPECT_EQ((*rr)[1], 201);
    EXPECT_FALSE(rx.takeReceiverReport(clk.now()));  // not due yet
    clk.advance(Millis(1001));
    EXPECT_TRUE(rx.takeReceiverReport(clk.now()));

    SenderReportInfo sr;
    sr.ssrc = p.ssrc;
    sr.ntpMsw = 0xAABBCCDD;
    sr.ntpLsw = 0x11223344;
    rx.onRtcp(buildSenderReport(sr), clk.now());
    clk.advance(Millis(2000));
    auto rr2 = rx.takeReceiverReport(clk.now());
    ASSERT_TRUE(rr2);
    // LSR = middle 32 bits of the NTP timestamp (bytes 24..27 of the compound packet)
    EXPECT_EQ((*rr2)[24], 0xCC);
    EXPECT_EQ((*rr2)[25], 0xDD);
    EXPECT_EQ((*rr2)[26], 0x11);
    EXPECT_EQ((*rr2)[27], 0x22);
    // DLSR ~ 2 s = 131072 / 65536 (bytes 28..31)
    std::uint32_t dlsr = ((*rr2)[28] << 24) | ((*rr2)[29] << 16) | ((*rr2)[30] << 8) | (*rr2)[31];
    EXPECT_NEAR(double(dlsr) / 65536.0, 2.0, 0.01);
}

TEST(StreamReceiver, NoReportBeforeAnyMedia) {
    StreamReceiverConfig rc;
    StreamReceiver rx(rc);
    EXPECT_FALSE(rx.takeReceiverReport(t0()));
}

TEST(StreamReceiver, ByeIsRecorded) {
    StreamReceiverConfig rc;
    StreamReceiver rx(rc);
    rx.onRtcp(Bytes{0x81, 203, 0, 1, 0, 0, 0, 9}, t0());
    EXPECT_TRUE(rx.snapshot(t0()).byeReceived);
}
