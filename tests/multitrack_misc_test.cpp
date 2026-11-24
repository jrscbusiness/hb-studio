#include <gtest/gtest.h>

#include "core/json.hpp"
#include "multitrack/edl_export.hpp"
#include "multitrack/resource_estimator.hpp"
#include "multitrack/session_manifest.hpp"
#include "multitrack/track_timeline.hpp"

using namespace hb::multitrack;
using hb::core::BudgetVerdict;
using hb::core::Tier;

TEST(MtEstimator, AutoBitrateAndUnits) {
    EXPECT_NEAR(autoVideoBitrateKbps(1920, 1080, 30), 8000, 500);
    EXPECT_EQ(autoVideoBitrateKbps(0, 0, 0), 8000);
    EXPECT_EQ(autoVideoBitrateKbps(7680, 4320, 60), 50000);   // clamped
    VideoPlan p;
    EXPECT_NEAR(encodeUnitsFor(p, false), 1.0, 1e-9);
    EXPECT_NEAR(encodeUnitsFor(p, true), 0.35, 1e-9);
    p.width = 3840; p.height = 2160;
    EXPECT_NEAR(encodeUnitsFor(p, false), 4.0, 1e-9);
}

TEST(MtEstimator, NothingSelectedIsFree) {
    EstimateInput in;
    const Estimate e = estimate(in);
    EXPECT_EQ(e.verdict, BudgetVerdict::Ok);
    EXPECT_EQ(e.videoTracks, 0);
    EXPECT_NEAR(e.videoMBps, 0, 1e-9);
}

TEST(MtEstimator, DiskRateAddsUp) {
    EstimateInput in;
    in.video.assign(2, VideoPlan{1920, 1080, 30, 8000});
    in.audio.assign(4, AudioPlan{48000, 2, 24, 0});
    in.programKbps = 8000;
    in.tier = Tier::Pro;
    const Estimate e = estimate(in);
    EXPECT_NEAR(e.videoMBps, 2.0, 1e-9);                 // 2 x 1 MB/s
    EXPECT_NEAR(e.audioMBps, 4 * 0.288, 1e-9);           // 48k * 2ch * 3 bytes
    EXPECT_NEAR(e.programMBps, 1.0, 1e-9);
    EXPECT_NEAR(e.totalMBps, 2.0 + 1.152 + 1.0, 1e-9);
    EXPECT_NEAR(e.gbPerHour, e.totalMBps * 3.6, 1e-9);
}

TEST(MtEstimator, CapsFollowTier) {
    VideoPlan p;
    EXPECT_EQ(maxSafeVideoTracks(Tier::Low, false, p, 1), 0);     // 1.5 - 1.0 = 0.5 unit left
    EXPECT_EQ(maxSafeVideoTracks(Tier::Medium, false, p, 1), 2);
    EXPECT_EQ(maxSafeVideoTracks(Tier::High, false, p, 1), 5);
    EXPECT_EQ(maxSafeVideoTracks(Tier::Pro, false, p, 1), 9);
    EXPECT_GT(maxSafeVideoTracks(Tier::Medium, true, p, 1), maxSafeVideoTracks(Tier::Medium, false, p, 1));
    EstimateInput in;
    in.tier = Tier::Medium;
    in.video.assign(2, VideoPlan{});
    EXPECT_EQ(estimate(in).cpuVerdict, BudgetVerdict::Ok);
    in.video.assign(3, VideoPlan{});
    EXPECT_EQ(estimate(in).cpuVerdict, BudgetVerdict::Warning);     // 4 units vs budget 3 (<= 4.5)
    in.video.assign(6, VideoPlan{});
    EXPECT_EQ(estimate(in).cpuVerdict, BudgetVerdict::Blocked);
    in.expertOverride = true;
    EXPECT_EQ(estimate(in).cpuVerdict, BudgetVerdict::Warning);
}

TEST(MtEstimator, DiskGuardVerdicts) {
    EstimateInput in;
    in.tier = Tier::Pro;
    in.video.assign(1, VideoPlan{});
    in.freeBytes = 3ull << 30;
    in.minFreeBytes = 2ull << 30;
    EXPECT_EQ(estimate(in).diskVerdict, BudgetVerdict::Warning);    // ~1 GiB usable at ~2 MB/s = ~9 min
    in.freeBytes = 2ull << 30;
    EXPECT_EQ(estimate(in).diskVerdict, BudgetVerdict::Blocked);
    in.freeBytes = 100ull << 30;
    EXPECT_EQ(estimate(in).diskVerdict, BudgetVerdict::Ok);
    in.freeBytes = 0;   // unknown
    const Estimate e = estimate(in);
    EXPECT_EQ(e.diskVerdict, BudgetVerdict::Ok);
    EXPECT_LT(e.minutesOfDisk, 0);
}

TEST(MtEdl, DropFrameTimecode) {
    EXPECT_EQ(framesToTimecode(0, 30000, 1001, true), "00:00:00;00");
    EXPECT_EQ(framesToTimecode(1799, 30000, 1001, true), "00:00:59;29");
    EXPECT_EQ(framesToTimecode(1800, 30000, 1001, true), "00:01:00;02");   // frames 00 and 01 are dropped
    EXPECT_EQ(framesToTimecode(17982, 30000, 1001, true), "00:10:00;00");  // every 10th minute keeps them
    EXPECT_EQ(framesToTimecode(17982 * 6, 30000, 1001, true), "01:00:00;00");
    EXPECT_EQ(framesToTimecode(3600, 60000, 1001, true), "00:01:00;04");   // 59.94 drops 4
    EXPECT_EQ(framesToTimecode(1800, 30, 1, true), "00:01:00:00");         // 30.00: never drop
    EXPECT_EQ(framesToTimecode(24 * 3600 + 5, 24, 1, false), "01:00:00:05");
    EXPECT_TRUE(usesDropFrame(30000, 1001));
    EXPECT_FALSE(usesDropFrame(25, 1));
}

namespace {
SessionManifest sample() {
    SessionManifest m;
    m.sessionName = "2026-11-24_14-05-33";
    m.startWallUnixMs = 1795529133250ll;
    m.durationNs = 10 * kNsPerSec;
    ManifestTrack v;
    v.id = "CAM01"; v.kind = "video"; v.name = "Cam \"A\""; v.width = 1920; v.height = 1080; v.fpsNum = 25; v.fpsDen = 1;
    v.codec = "h264"; v.state = "finished";
    ManifestSegment s1; s1.index = 1; s1.file = "iso/CAM01_a.mp4"; s1.startNs = 40000000; s1.startUnit = 1; s1.units = 250; s1.closed = true;
    ManifestSegment s2 = s1; s2.index = 2; s2.file = "iso/CAM01_a_002.mp4"; s2.startNs = 10040000000ll; s2.startUnit = 251; s2.units = 25;
    v.segments = {s1, s2};
    v.startOffsetNs = 40000000;
    ManifestTrack a;
    a.id = "CH01"; a.kind = "audio"; a.name = "MIC 1"; a.sampleRate = 48000; a.channels = 2; a.bitsPerSample = 24; a.tap = "pre-fader";
    a.state = "finished"; a.codec = "pcm_s24le";
    ManifestSegment sa; sa.index = 1; sa.file = "audio/CH01_mic.wav"; sa.units = 480000; sa.closed = true;
    a.segments = {sa};
    m.tracks = {v, a};
    return m;
}
}  // namespace

TEST(MtManifest, JsonIsValidAndComplete) {
    const std::string text = manifestToJson(sample());
    hb::core::json::Value root;
    ASSERT_TRUE(hb::core::json::parse(text, root)) << text;
    EXPECT_EQ(root.get("schema")->n, 1);
    EXPECT_EQ(root.get("startWallClockUtc")->s, "2026-11-24T14:05:33.250Z");
    const auto* tracks = root.get("tracks");
    ASSERT_TRUE(tracks && tracks->a.size() == 2);
    EXPECT_EQ(tracks->a[0].get("name")->s, "Cam \"A\"");
    EXPECT_EQ(tracks->a[0].get("segments")->a.size(), 2u);
    EXPECT_EQ(tracks->a[0].get("startOffsetNs")->n, 40000000);
    EXPECT_EQ(tracks->a[1].get("tap")->s, "pre-fader");
    EXPECT_EQ(tracks->a[1].get("sampleRate")->n, 48000);
}

TEST(MtManifest, IsoDate) {
    EXPECT_EQ(unixMsToIsoUtc(0), "1970-01-01T00:00:00.000Z");
    EXPECT_EQ(unixMsToIsoUtc(951782400000ll), "2000-02-29T00:00:00.000Z");
    EXPECT_EQ(unixMsToIsoUtc(-1), "1969-12-31T23:59:59.999Z");
}

TEST(MtEdl, BuildsOneListPerTrack) {
    const auto files = buildSessionEdls(sample(), 30, 1);
    ASSERT_EQ(files.size(), 2u);
    EXPECT_EQ(files[0].relativePath, "edl/CAM01.edl");
    const std::string& c = files[0].content;
    EXPECT_NE(c.find("FCM: NON-DROP FRAME"), std::string::npos);
    // event 1: 250 frames at 25 fps = 10 s; record-in = 40 ms = frame 1
    EXPECT_NE(c.find("001  CAM01    V     C"), std::string::npos);
    EXPECT_NE(c.find("00:00:00:00 00:00:10:00 00:00:00:01 00:00:10:01"), std::string::npos) << c;
    EXPECT_NE(c.find("* FROM CLIP NAME: CAM01_a.mp4"), std::string::npos);
    // event 2 starts at 10.04 s = frame 251
    EXPECT_NE(c.find("002  CAM01"), std::string::npos);
    EXPECT_NE(c.find("00:00:10:01 00:00:11:01"), std::string::npos) << c;
    EXPECT_EQ(files[1].relativePath, "edl/CH01.edl");
    // 480000 samples at 48k = 10 s = 300 frames at the 30 fps timeline
    EXPECT_NE(files[1].content.find("00:00:00:00 00:00:10:00 00:00:00:00 00:00:10:00"), std::string::npos) << files[1].content;
    EXPECT_NE(files[1].content.find(" AA "), std::string::npos);
    EXPECT_NE(c.find("\r\n"), std::string::npos);
}
