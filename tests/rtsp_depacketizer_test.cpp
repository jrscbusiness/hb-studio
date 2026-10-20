#include <gtest/gtest.h>

#include <random>

#include "rtsp/depacketizer.hpp"
#include "rtsp_test_support.hpp"

using namespace hb::rtsp;
using namespace hb::rtsp::test;

namespace {
struct Rig {
    std::unique_ptr<Depacketizer> d;
    std::vector<AccessUnit> out;
    explicit Rig(DepacketizerConfig cfg) : d(Depacketizer::create(cfg)) {
        d->setSink([this](AccessUnit&& a) { out.push_back(std::move(a)); });
    }
    void feed(const Bytes& rtp) {
        auto v = parseRtp(rtp);
        ASSERT_TRUE(v.has_value());
        d->push(v->sequence, v->timestamp, v->marker, v->payload, t0());
    }
    void feed(const std::vector<Bytes>& pkts) {
        for (auto& p : pkts) feed(p);
    }
};

DepacketizerConfig cfg264() {
    DepacketizerConfig c;
    c.codec = Codec::H264;
    c.paramSets = {sps264(), pps264()};
    return c;
}
DepacketizerConfig cfg265() {
    DepacketizerConfig c;
    c.codec = Codec::H265;
    c.paramSets = {vps265(), sps265(), pps265()};
    return c;
}
}  // namespace

TEST(H264Depack, SingleNalKeyframeGetsParamSetsInjected) {
    Rig r(cfg264());
    Packetizer p;
    r.feed(p.h264({idr264(40)}, 9000));
    ASSERT_EQ(r.out.size(), 1u);
    EXPECT_TRUE(r.out[0].keyframe);
    EXPECT_FALSE(r.out[0].damaged);
    EXPECT_EQ(r.out[0].data, annexB({sps264(), pps264(), idr264(40)}));
    EXPECT_EQ(r.d->stats().paramSetInjections, 1u);
}

TEST(H264Depack, StapAAndFuARoundTripExact) {
    Rig r(cfg264());
    Packetizer p;
    p.mtu = 120;
    Bytes bigIdr = idr264(1000);
    auto pkts = p.h264({sps264(), pps264(), bigIdr}, 9000);
    EXPECT_GT(pkts.size(), 5u);  // STAP-A + many FU-A
    r.feed(pkts);
    ASSERT_EQ(r.out.size(), 1u);
    EXPECT_EQ(r.out[0].data, annexB({sps264(), pps264(), bigIdr}));  // in-band params: not duplicated
    EXPECT_EQ(r.d->stats().paramSetInjections, 0u);
    EXPECT_TRUE(r.out[0].keyframe);
}

TEST(H264Depack, PFramesFollowAndEarlyPFramesAreDropped) {
    Rig r(cfg264());
    Packetizer p;
    r.feed(p.h264({p264(30)}, 3000));  // before any keyframe: undecodable
    EXPECT_TRUE(r.out.empty());
    EXPECT_EQ(r.d->stats().droppedAus, 1u);
    r.feed(p.h264({idr264(), }, 6000));
    r.feed(p.h264({p264(30)}, 9600));
    r.feed(p.h264({p264(31)}, 13200));
    ASSERT_EQ(r.out.size(), 3u);
    EXPECT_FALSE(r.out[1].keyframe);
    EXPECT_EQ(r.out[1].data, annexB({p264(30)}));
    EXPECT_EQ(r.d->stats().keyframes, 1u);
}

TEST(H264Depack, LostFuFragmentDropsAuAndWaitsForNextKeyframe) {
    Rig r(cfg264());
    Packetizer p;
    p.mtu = 100;
    r.feed(p.h264({idr264(600)}, 1000));  // ok
    auto bad = p.h264({idr264(600, 5)}, 4600);
    ASSERT_GT(bad.size(), 4u);
    bad.erase(bad.begin() + 2);  // lose a middle fragment
    r.feed(bad);
    r.feed(p.h264({p264()}, 8200));  // reference chain is broken: dropped
    r.feed(p.h264({p264()}, 11800));
    EXPECT_EQ(r.out.size(), 1u);
    EXPECT_TRUE(r.d->waitingForKeyframe());
    r.feed(p.h264({idr264(600, 9)}, 15400));  // recovery
    ASSERT_EQ(r.out.size(), 2u);
    EXPECT_TRUE(r.out[1].keyframe);
    EXPECT_FALSE(r.out[1].damaged);
    EXPECT_EQ(r.d->stats().seqGaps, 1u);
    EXPECT_GE(r.d->stats().damagedAus, 1u);
    EXPECT_GE(r.d->stats().discardedFragments, 1u);
    // The recovered keyframe must be bit-exact (no leftovers of the aborted NAL).
    EXPECT_EQ(r.out[1].data, annexB({sps264(), pps264(), idr264(600, 9)}));
}

TEST(H264Depack, LostWholePacketAtAuBoundaryIsDetected) {
    Rig r(cfg264());
    Packetizer p;
    r.feed(p.h264({idr264()}, 1000));
    r.feed(p.h264({p264()}, 4600));
    p.h264({p264()}, 8200);  // generated but "lost on the wire": sequence gap
    r.feed(p.h264({p264()}, 11800));
    EXPECT_EQ(r.out.size(), 2u);  // the AU after the gap is not trusted
    EXPECT_EQ(r.d->stats().seqGaps, 1u);
}

TEST(H264Depack, EmitDamagedWhenConfigured) {
    auto c = cfg264();
    c.dropUntilKeyframeAfterLoss = false;
    Rig r(c);
    Packetizer p;
    p.mtu = 100;
    r.feed(p.h264({idr264(600)}, 1000));
    Bytes sei{0x06, 0x05, 0x01, 0x02};
    auto bad = p.h264({sei, p264(400)}, 4600);  // [SEI][FU-A start][FU-A ...]
    ASSERT_GT(bad.size(), 3u);
    bad.erase(bad.begin() + 2);  // lose a middle fragment of the P slice
    r.feed(bad);
    r.feed(p.h264({p264()}, 8200));
    ASSERT_EQ(r.out.size(), 3u);
    EXPECT_TRUE(r.out[1].damaged);
    EXPECT_EQ(r.out[1].data, annexB({sei}));  // the incomplete slice is not in the output
    EXPECT_FALSE(r.out[2].damaged);           // the flag marks AUs with missing packets, not their dependents
    EXPECT_EQ(r.d->stats().seqGaps, 1u);
}

TEST(H264Depack, DuplicatePacketsAreIgnored) {
    Rig r(cfg264());
    Packetizer p;
    p.mtu = 100;
    auto pkts = p.h264({sps264(), pps264(), idr264(500)}, 1000);
    for (auto& pk : pkts) {
        r.feed(pk);
        r.feed(pk);  // immediate duplicate
    }
    ASSERT_EQ(r.out.size(), 1u);
    EXPECT_EQ(r.out[0].data, annexB({sps264(), pps264(), idr264(500)}));
    EXPECT_EQ(r.d->stats().oldOrDuplicate, pkts.size());
    EXPECT_FALSE(r.out[0].damaged);
}

TEST(H264Depack, MissingMarkerFlushedByTimestampChange) {
    Rig r(cfg264());
    Packetizer p;
    auto a = p.h264({idr264()}, 1000);
    // clear marker bit of the only packet
    a[0][1] &= 0x7f;
    r.feed(a);
    EXPECT_TRUE(r.out.empty());
    r.feed(p.h264({p264()}, 4600));
    ASSERT_EQ(r.out.size(), 2u);
    EXPECT_TRUE(r.out[0].keyframe);
}

TEST(H264Depack, AudStaysFirstAndParamsFollowIt) {
    Rig r(cfg264());
    Packetizer p;
    Bytes aud{0x09, 0xF0};
    r.feed(p.h264({aud, idr264(10)}, 1000));
    ASSERT_EQ(r.out.size(), 1u);
    EXPECT_EQ(r.out[0].data, annexB({aud, sps264(), pps264(), idr264(10)}));
}

TEST(H264Depack, InBandParameterUpdateIsUsedForLaterKeyframes) {
    Rig r(cfg264());
    Packetizer p;
    Bytes sps2 = sps264();
    sps2.push_back(0x99);
    r.feed(p.h264({sps2, pps264(), idr264()}, 1000));
    r.feed(p.h264({idr264(20)}, 5000));  // no params in band: injected from memory
    ASSERT_EQ(r.out.size(), 2u);
    Bytes injected = annexB({sps2});
    auto& d = r.out[1].data;
    EXPECT_NE(std::search(d.begin(), d.end(), injected.begin(), injected.end()), d.end());
}

TEST(H264Depack, TruncatedAndInvalidPayloadsAreCountedNotFatal) {
    Rig r(cfg264());
    Packetizer p;
    auto push = [&](Bytes payload, std::uint32_t ts) { r.feed(p.emit(true, ts, payload)); };
    push({}, 100);                                   // empty
    push({0x80 | 0x65, 1, 2}, 200);                  // forbidden bit
    push({0x1c}, 300);                               // FU-A without header
    push({0x1c, 0x85}, 400);                         // FU-A start without data
    push({0x18, 0x00}, 500);                         // STAP-A truncated size
    push({0x18, 0x00, 0x09, 0x67, 0x42}, 600);       // STAP-A size beyond payload
    push({0x18, 0x00, 0x00}, 700);                   // zero size
    push({0x19, 0, 0}, 800);                         // STAP-B: unsupported
    push({0x1c, 0xC5, 1, 2}, 900);                   // FU-A with S and E both set
    push({0x00}, 1000);                              // type 0
    EXPECT_TRUE(r.out.empty());
    EXPECT_GE(r.d->stats().truncatedPayloads, 6u);
    EXPECT_GE(r.d->stats().unsupportedNals, 2u);
    // Still works afterwards.
    r.feed(p.h264({idr264()}, 5000));
    EXPECT_EQ(r.out.size(), 1u);
}

TEST(H264Depack, OversizedAccessUnitIsDroppedAndStreamRecovers) {
    auto c = cfg264();
    c.maxAuBytes = 2000;
    Rig r(c);
    Packetizer p;
    p.mtu = 400;
    r.feed(p.h264({idr264(5000)}, 1000));
    EXPECT_TRUE(r.out.empty());
    EXPECT_GE(r.d->stats().oversizeAus, 1u);
    r.feed(p.h264({idr264(100)}, 5000));
    EXPECT_EQ(r.out.size(), 1u);
}

TEST(H264Depack, PtsFollowsTimestampAndSurvivesWrap) {
    Rig r(cfg264());
    Packetizer p;
    r.feed(p.h264({idr264()}, 0xFFFFF000u));
    r.feed(p.h264({p264()}, 0xFFFFF000u + 3600));  // +40 ms
    r.feed(p.h264({p264()}, 0xFFFFF000u + 7200));  // crosses 2^32 wrap
    ASSERT_EQ(r.out.size(), 3u);
    EXPECT_EQ(r.out[0].ptsUs, 0);
    EXPECT_EQ(r.out[1].ptsUs, 40000);
    EXPECT_EQ(r.out[2].ptsUs, 80000);
    EXPECT_EQ(r.out[2].rtpTimestamp, 0xFFFFF000u + 7200);
}

TEST(H264Depack, SequenceWraparoundIsNotLoss) {
    Rig r(cfg264());
    Packetizer p;
    p.seq = 65534;
    for (int i = 0; i < 4; ++i) r.feed(p.h264({i == 0 ? idr264() : p264()}, 1000 + 3600 * i));
    EXPECT_EQ(r.out.size(), 4u);
    EXPECT_EQ(r.d->stats().seqGaps, 0u);
}

TEST(H264Depack, FuzzRandomPayloadsNeverCrash) {
    std::mt19937 g(77);
    Rig r(cfg264());
    std::uint16_t seq = 0;
    for (int i = 0; i < 20000; ++i) {
        Bytes pl(g() % 80);
        for (auto& b : pl) b = static_cast<std::uint8_t>(g());
        if (!pl.empty() && g() % 2) pl[0] = static_cast<std::uint8_t>((pl[0] & 0xe0) | (g() % 2 ? 28 : 24));
        if (g() % 7 == 0) ++seq;  // random gaps
        r.d->push(seq++, static_cast<std::uint32_t>(g() % 5) * 3600, g() % 3 == 0, pl, t0());
    }
    SUCCEED();
}

// ---------------------------------------------------------------- H.265
TEST(H265Depack, KeyframeWithApAndFuRoundTrip) {
    Rig r(cfg265());
    Packetizer p;
    p.mtu = 150;
    Bytes big = idr265(900);
    r.feed(p.h265({vps265(), sps265(), pps265(), big}, 1000));
    ASSERT_EQ(r.out.size(), 1u);
    EXPECT_TRUE(r.out[0].keyframe);
    EXPECT_EQ(r.out[0].data, annexB({vps265(), sps265(), pps265(), big}));
    EXPECT_EQ(r.d->stats().paramSetInjections, 0u);
}

TEST(H265Depack, InjectsVpsSpsPpsBeforeIrap) {
    Rig r(cfg265());
    Packetizer p;
    r.feed(p.h265({idr265(30)}, 1000));
    r.feed(p.h265({p265()}, 4600));
    ASSERT_EQ(r.out.size(), 2u);
    EXPECT_EQ(r.out[0].data, annexB({vps265(), sps265(), pps265(), idr265(30)}));
    EXPECT_EQ(r.out[1].data, annexB({p265()}));
    EXPECT_FALSE(r.out[1].keyframe);
}

TEST(H265Depack, LossInFuDropsUntilNextIrap) {
    Rig r(cfg265());
    Packetizer p;
    p.mtu = 100;
    r.feed(p.h265({idr265(500)}, 1000));
    auto bad = p.h265({idr265(500, 4)}, 4600);
    bad.erase(bad.begin() + 3);
    r.feed(bad);
    r.feed(p.h265({p265()}, 8200));
    EXPECT_EQ(r.out.size(), 1u);
    r.feed(p.h265({idr265(500, 8)}, 11800));
    ASSERT_EQ(r.out.size(), 2u);
    EXPECT_EQ(r.out[1].data, annexB({vps265(), sps265(), pps265(), idr265(500, 8)}));
}

TEST(H265Depack, MalformedPayloadsCountedAndFuzzSafe) {
    Rig r(cfg265());
    Packetizer p;
    r.feed(p.emit(true, 100, {0x62}));                   // too short
    r.feed(p.emit(true, 200, {0x62, 0x01, 0x80}));       // FU start w/o data is tolerated
    r.feed(p.emit(true, 300, {static_cast<std::uint8_t>(48 << 1), 1, 0, 9, 1}));  // AP size beyond payload
    r.feed(p.emit(true, 400, {static_cast<std::uint8_t>(50 << 1), 1, 0}));        // PACI
    EXPECT_TRUE(r.out.empty());
    EXPECT_GE(r.d->stats().truncatedPayloads, 2u);
    EXPECT_GE(r.d->stats().unsupportedNals, 1u);
    std::mt19937 g(5);
    std::uint16_t seq = 5000;
    for (int i = 0; i < 20000; ++i) {
        Bytes pl(g() % 80);
        for (auto& b : pl) b = static_cast<std::uint8_t>(g());
        if (pl.size() > 1 && g() % 2) pl[0] = static_cast<std::uint8_t>((pl[0] & 0x81) | ((g() % 2 ? 48 : 49) << 1));
        if (g() % 7 == 0) ++seq;
        r.d->push(seq++, static_cast<std::uint32_t>(g() % 5) * 3600, g() % 3 == 0, pl, t0());
    }
    SUCCEED();
}
