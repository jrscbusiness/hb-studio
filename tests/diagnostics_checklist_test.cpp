#include <gtest/gtest.h>

#include "diagnostics/checklist.hpp"
#include "diagnostics/settings.hpp"

using namespace hb::diag;

namespace {
constexpr std::int64_t GiB = 1024LL * 1024 * 1024;

const CheckResult& find(const std::vector<CheckResult>& v, const std::string& id) {
    for (const auto& r : v)
        if (r.id == id) return r;
    static CheckResult none;
    ADD_FAILURE() << "missing check " << id;
    return none;
}

Probes goodProbes() {
    Probes p;
    p.freeDiskBytes = [](const std::string&) { return 500 * GiB; };
    p.reachability = [] { NetProbeResult n; n.reachable = true; n.serverEpochSec = 1800000000; return n; };
    p.uploadTest = [](const std::string&) { UploadProbeResult u; u.ok = true; u.kbps = 20000; return u; };
    p.encoders = [] { EncoderProbeResult e; e.known = true; e.hardwareH264 = true; e.softwareH264 = true; return e; };
    p.nowEpochSec = [] { return std::int64_t(1800000010); };
    return p;
}

ChecklistInput goodInput() {
    ChecklistInput in;
    in.recordingFolder = "D:\\Rec";
    in.recordingVideoKbps = 8000;
    in.recordingAudioKbps = 192;
    in.minFreeBytes = 500LL * 1024 * 1024;
    in.destinations = {{"YT", true, true, true, 6000, 160}};
    in.cameras = {{0, "Cam A", true, true, ""}, {1, "Cam B", true, true, ""}};
    in.audio = {{0, "Mic", true, "online", -20}};
    in.speedTestEnabled = true;
    in.speedTestUrl = "https://speed.example.com/up";
    return in;
}
}  // namespace

TEST(DiagChecklist, AllGoodIsAllOk) {
    const auto r = runChecklist(goodInput(), goodProbes());
    ASSERT_EQ(r.size(), 8u);
    for (const auto& c : r) EXPECT_EQ(c.status, CheckStatus::Ok) << c.id << ": " << c.detail;
    EXPECT_NE(find(r, "disk").detail.find("min de gravação"), std::string::npos);
}

TEST(DiagChecklist, DiskThresholdsAndMinutesEstimate) {
    ChecklistInput in = goodInput();   // 8192 kbps ~ 1.024 MB/s ~ 61.4 MB/min
    Probes p = goodProbes();
    p.freeDiskBytes = [](const std::string&) { return GiB; };   // ~ 17 min
    EXPECT_EQ(checkDisk(in, p).status, CheckStatus::Fail);
    p.freeDiskBytes = [](const std::string&) { return 5 * GiB; };   // ~ 87 min
    EXPECT_EQ(checkDisk(in, p).status, CheckStatus::Warn);
    p.freeDiskBytes = [](const std::string&) { return 400LL * 1024 * 1024; };   // below the recorder's own minimum
    EXPECT_EQ(checkDisk(in, p).status, CheckStatus::Fail);
    p.freeDiskBytes = [](const std::string&) { return std::int64_t(-1); };
    EXPECT_EQ(checkDisk(in, p).status, CheckStatus::Skip);
    p.freeDiskBytes = nullptr;
    EXPECT_EQ(checkDisk(in, p).status, CheckStatus::Skip);
    in.recordingFolder.clear();
    EXPECT_EQ(checkDisk(in, goodProbes()).status, CheckStatus::Warn);
}

TEST(DiagChecklist, NoInternetFailsAndSkipsUpload) {
    Probes p = goodProbes();
    p.reachability = [] { NetProbeResult n; n.error = "timeout"; return n; };
    bool uploaded = false;
    p.uploadTest = [&](const std::string&) { uploaded = true; return UploadProbeResult{}; };
    const auto r = runChecklist(goodInput(), p);
    EXPECT_EQ(find(r, "net").status, CheckStatus::Fail);
    EXPECT_FALSE(find(r, "net").hint.empty());
    EXPECT_EQ(find(r, "upload").status, CheckStatus::Skip);
    EXPECT_FALSE(uploaded);
}

TEST(DiagChecklist, UploadComparedToSumOfEnabledDestinations) {
    ChecklistInput in = goodInput();
    in.destinations = {{"A", true, true, true, 6000, 160}, {"B", true, true, true, 4000, 128}, {"Off", false, true, true, 9000, 128}};
    const int need = 6160 + 4128;
    NetProbeResult net;
    net.reachable = true;
    Probes p = goodProbes();
    p.uploadTest = [&](const std::string&) { UploadProbeResult u; u.ok = true; u.kbps = need - 1; return u; };
    EXPECT_EQ(checkUpload(in, net, p).status, CheckStatus::Fail);
    p.uploadTest = [&](const std::string&) { UploadProbeResult u; u.ok = true; u.kbps = need * 1.2; return u; };
    EXPECT_EQ(checkUpload(in, net, p).status, CheckStatus::Warn);
    p.uploadTest = [&](const std::string&) { UploadProbeResult u; u.ok = true; u.kbps = need * 2; return u; };
    EXPECT_EQ(checkUpload(in, net, p).status, CheckStatus::Ok);
    p.uploadTest = [](const std::string&) { UploadProbeResult u; u.error = "HTTP 403"; return u; };
    EXPECT_EQ(checkUpload(in, net, p).status, CheckStatus::Warn);
    EXPECT_NE(checkUpload(in, net, p).detail.find(std::to_string(need)), std::string::npos);
}

TEST(DiagChecklist, SpeedTestIsOffByDefaultAndNeverRunsUnlessEnabledWithValidUrl) {
    ChecklistInput in = goodInput();
    EXPECT_FALSE(ChecklistInput{}.speedTestEnabled);
    int calls = 0;
    Probes p = goodProbes();
    p.uploadTest = [&](const std::string&) { ++calls; return UploadProbeResult{}; };
    NetProbeResult net;
    net.reachable = true;
    in.speedTestEnabled = false;
    EXPECT_EQ(checkUpload(in, net, p).status, CheckStatus::Skip);
    in.speedTestEnabled = true;
    in.speedTestUrl = "http://insecure.example.com/";
    EXPECT_EQ(checkUpload(in, net, p).status, CheckStatus::Warn);
    in.speedTestUrl = "https://user:pw@host/";
    EXPECT_EQ(checkUpload(in, net, p).status, CheckStatus::Warn);
    EXPECT_EQ(calls, 0);
}

TEST(DiagChecklist, UrlValidation) {
    EXPECT_TRUE(isValidSpeedTestUrl("https://example.com/upload?x=1"));
    EXPECT_TRUE(isValidSpeedTestUrl("HTTPS://example.com:8443"));
    EXPECT_FALSE(isValidSpeedTestUrl("http://example.com"));
    EXPECT_FALSE(isValidSpeedTestUrl("https://"));
    EXPECT_FALSE(isValidSpeedTestUrl("https://a@b.com"));
    EXPECT_FALSE(isValidSpeedTestUrl("https://exa mple.com"));
    EXPECT_FALSE(isValidSpeedTestUrl("https://example.com/\n"));
    EXPECT_FALSE(isValidSpeedTestUrl("https://" + std::string(400, 'a')));
    EXPECT_FALSE(isValidSpeedTestUrl("file:///c:/x"));
}

TEST(DiagChecklist, CamerasOnlineVsAssigned) {
    ChecklistInput in = goodInput();
    EXPECT_EQ(checkCameras(in).status, CheckStatus::Ok);
    in.cameras[1].online = false;
    auto r = checkCameras(in);
    EXPECT_EQ(r.status, CheckStatus::Warn);
    EXPECT_NE(r.detail.find("1 de 2"), std::string::npos);
    EXPECT_NE(r.detail.find("CAM 2"), std::string::npos);
    in.cameras[0].online = false;
    EXPECT_EQ(checkCameras(in).status, CheckStatus::Fail);
    in.cameras[0].enabled = in.cameras[1].enabled = false;
    EXPECT_EQ(checkCameras(in).status, CheckStatus::Warn);
    in.cameras.clear();
    EXPECT_EQ(checkCameras(in).status, CheckStatus::Warn);
}

TEST(DiagChecklist, AudioPresenceAndSignal) {
    ChecklistInput in = goodInput();
    EXPECT_EQ(checkAudio(in).status, CheckStatus::Ok);
    in.audio.push_back({1, "Line", true, "online", -80});
    EXPECT_EQ(checkAudio(in).status, CheckStatus::Warn);
    in.audio.push_back({2, "Gone", true, "error", -200});
    EXPECT_EQ(checkAudio(in).status, CheckStatus::Fail);
    in.audio = {{0, "Unassigned", false, "off", -200}};
    EXPECT_EQ(checkAudio(in).status, CheckStatus::Warn);
}

TEST(DiagChecklist, EncoderHardwareSoftwareOrUnknown) {
    Probes p = goodProbes();
    EXPECT_EQ(checkEncoder(p).status, CheckStatus::Ok);
    p.encoders = [] { EncoderProbeResult e; e.known = true; e.softwareH264 = true; return e; };
    EXPECT_EQ(checkEncoder(p).status, CheckStatus::Warn);
    p.encoders = [] { EncoderProbeResult e; e.known = true; return e; };
    EXPECT_EQ(checkEncoder(p).status, CheckStatus::Fail);
    p.encoders = [] { return EncoderProbeResult{}; };
    EXPECT_EQ(checkEncoder(p).status, CheckStatus::Skip);
    p.encoders = nullptr;
    EXPECT_EQ(checkEncoder(p).status, CheckStatus::Skip);
}

TEST(DiagChecklist, DestinationsNeedUrlAndKeyWithoutRevealingIt) {
    ChecklistInput in = goodInput();
    EXPECT_EQ(checkDestinations(in).status, CheckStatus::Ok);
    in.destinations.push_back({"Twitch", true, true, false, 6000, 160});
    in.destinations.push_back({"Face", true, false, true, 6000, 160});
    in.destinations.push_back({"Disabled", false, false, false, 0, 0});
    const auto r = checkDestinations(in);
    EXPECT_EQ(r.status, CheckStatus::Fail);
    EXPECT_NE(r.detail.find("Twitch (sem chave)"), std::string::npos);
    EXPECT_NE(r.detail.find("Face (sem URL)"), std::string::npos);
    EXPECT_EQ(r.detail.find("Disabled"), std::string::npos);
    in.destinations.clear();
    EXPECT_EQ(checkDestinations(in).status, CheckStatus::Warn);
}

TEST(DiagChecklist, ClockSanity) {
    Probes p = goodProbes();
    NetProbeResult net;
    net.reachable = true;
    net.serverEpochSec = 1800000000;
    EXPECT_EQ(checkClock(net, p).status, CheckStatus::Ok);
    p.nowEpochSec = [] { return std::int64_t(1800000000 + 300); };
    EXPECT_EQ(checkClock(net, p).status, CheckStatus::Warn);
    p.nowEpochSec = [] { return std::int64_t(1800000000 - 3600); };
    EXPECT_EQ(checkClock(net, p).status, CheckStatus::Fail);
    p.nowEpochSec = [] { return std::int64_t(1000); };
    EXPECT_EQ(checkClock(net, p).status, CheckStatus::Fail);
    net.serverEpochSec = 0;
    p.nowEpochSec = [] { return std::int64_t(1800000000); };
    EXPECT_EQ(checkClock(net, p).status, CheckStatus::Ok);
    p.nowEpochSec = nullptr;
    EXPECT_EQ(checkClock(net, p).status, CheckStatus::Skip);
}

TEST(DiagChecklist, ThrowingProbeDoesNotAbortTheRun) {
    Probes p = goodProbes();
    p.freeDiskBytes = [](const std::string&) -> std::int64_t { throw 1; };
    p.encoders = []() -> EncoderProbeResult { throw std::runtime_error("x"); };
    const auto r = runChecklist(goodInput(), p);
    ASSERT_EQ(r.size(), 8u);
    EXPECT_EQ(find(r, "disk").status, CheckStatus::Skip);
    EXPECT_EQ(find(r, "encoder").status, CheckStatus::Skip);
    EXPECT_EQ(find(r, "cameras").status, CheckStatus::Ok);
}

TEST(DiagChecklist, NoProbesAtAllSkipsHonestly) {
    const auto r = runChecklist(goodInput(), Probes{});
    EXPECT_EQ(find(r, "disk").status, CheckStatus::Skip);
    EXPECT_EQ(find(r, "net").status, CheckStatus::Skip);
    EXPECT_EQ(find(r, "encoder").status, CheckStatus::Skip);
    EXPECT_EQ(find(r, "clock").status, CheckStatus::Skip);
}

TEST(DiagChecklist, JsonShapeEscapesAndCounts) {
    std::vector<CheckResult> v;
    CheckResult a;
    a.id = "x"; a.status = CheckStatus::Ok; a.title = "T \"q\""; a.detail = "d\n"; a.hint = "";
    CheckResult b = a;
    b.status = CheckStatus::Fail;
    v = {a, b};
    const std::string j = checklistJson(v);
    EXPECT_NE(j.find("\"t\":\"checklist\""), std::string::npos);
    EXPECT_NE(j.find("\"ok\":1,\"warn\":0,\"fail\":1"), std::string::npos);
    EXPECT_NE(j.find("T \\\"q\\\""), std::string::npos);
    EXPECT_NE(j.find("d\\n"), std::string::npos);
}

TEST(DiagSettings, DefaultsOffAndRoundTripAndHostile) {
    EXPECT_FALSE(ChecklistSettings{}.speedTestEnabled);
    ChecklistSettings s;
    s.speedTestEnabled = true;
    s.speedTestUrl = "https://speed.example.com/up";
    const auto back = ChecklistSettings::fromJson(s.toJson());
    EXPECT_TRUE(back.speedTestEnabled);
    EXPECT_EQ(back.speedTestUrl, s.speedTestUrl);
    // enabled without a valid URL is forced off; garbage yields defaults
    EXPECT_FALSE(ChecklistSettings::fromJson("{\"speedTestEnabled\":true,\"speedTestUrl\":\"http://x\"}").speedTestEnabled);
    EXPECT_FALSE(ChecklistSettings::fromJson("not json").speedTestEnabled);
    EXPECT_FALSE(ChecklistSettings::fromJson("").speedTestEnabled);
    EXPECT_NE(back.uiJson().find("\"t\":\"chkcfg\""), std::string::npos);
}
