#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <algorithm>
#include <sstream>
#include <thread>

#include "cloud/cloud_service.hpp"
#include "cloud/cloud_settings.hpp"
#include "cloud/file_feed.hpp"
#include "cloud/hash_util.hpp"
#include "cloud/loopback_listener.hpp"
#include "core/json.hpp"
#include "cloud_test_util.hpp"

using namespace hb::cloud;
using namespace cloudtest;

namespace {
std::string tempDir(const char* tag) {
    auto p = std::filesystem::temp_directory_path() / (std::string("hb_cloudsvc_") + tag + "_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()));
    std::error_code ec;
    std::filesystem::remove_all(p, ec);
    std::filesystem::create_directories(p, ec);
    return p.string();
}
void cleanup(const std::string& d) {
    std::error_code ec;
    std::filesystem::remove_all(d, ec);
}
}  // namespace

TEST(CloudFeed, EventBusParsing) {
    hb::core::Event e;
    e.topic = "recording.fileClosed";
    e.fields = {{"path", "/rec/a.mp4"}, {"kind", "iso-audio"}, {"index", "3"}};
    ClosedFile f;
    ASSERT_TRUE(EventBusFeed::parse(e, f));
    EXPECT_EQ(f.path, "/rec/a.mp4");
    EXPECT_EQ(f.kind, "iso-audio");
    EXPECT_EQ(f.index, 3);
    EXPECT_TRUE(f.closedByProducer);
    e.fields["kind"] = "bogus";
    EXPECT_FALSE(EventBusFeed::parse(e, f));
    e.fields = {{"kind", "program"}};
    EXPECT_FALSE(EventBusFeed::parse(e, f));   // no path
    e.fields = {{"path", "/x.wav"}};
    ASSERT_TRUE(EventBusFeed::parse(e, f));
    EXPECT_EQ(f.kind, "program");              // kind defaults to program
}

TEST(CloudFeed, BusSubscriptionDeliversAndUnsubscribes) {
    hb::core::EventBus bus;
    int got = 0;
    {
        EventBusFeed feed(bus);
        feed.setSink([&](const ClosedFile& f) { ++got; EXPECT_EQ(f.path, "/a.mp4"); });
        bus.publish({"recording.fileClosed", {{"path", "/a.mp4"}, {"kind", "program"}, {"index", "0"}}});
        bus.publish({"recording.other", {{"path", "/b.mp4"}}});
        EXPECT_EQ(got, 1);
    }
    bus.publish({"recording.fileClosed", {{"path", "/a.mp4"}}});   // feed destroyed: must not call into freed memory
    EXPECT_EQ(got, 1);
}

TEST(CloudFeed, FolderWatchSkipsHistoryAndWaitsForStability) {
    auto fs = std::make_shared<MemFs>();
    std::int64_t now = 1000;
    FolderWatchFeed::Options o;
    o.fs = fs;
    o.folder = [] { return std::string("/rec"); };
    o.wallNow = [&] { return now; };
    o.stableSeconds = 30;
    FolderWatchFeed w(o);
    std::vector<ClosedFile> got;
    w.setSink([&](const ClosedFile& f) { got.push_back(f); });
    fs->files["/rec/old.mp4"] = {"x", 10};
    w.poll();                       // first scan: old recordings are NOT uploaded
    now += 100;
    w.poll();
    EXPECT_TRUE(got.empty());
    fs->files["/rec/new.mp4"] = {"abc", now};
    fs->files["/rec/new_iso2.wav"] = {"abc", now};
    fs->files["/rec/notes.txt"] = {"abc", now};
    w.poll();
    now += 10;
    fs->files["/rec/new.mp4"].data += "d";   // still growing
    fs->files["/rec/new.mp4"].mtime = now;
    w.poll();
    now += 31;
    w.poll();
    ASSERT_EQ(got.size(), 2u);               // new_iso2.wav was stable the whole time; new.mp4 restarted its timer
    std::sort(got.begin(), got.end(), [](auto& a, auto& b) { return a.path < b.path; });
    EXPECT_EQ(got[0].path, "/rec/new.mp4");
    EXPECT_EQ(got[0].kind, "program");
    EXPECT_FALSE(got[0].closedByProducer);
    EXPECT_EQ(got[1].kind, "iso-audio");
    now += 100;
    w.poll();
    EXPECT_EQ(got.size(), 2u);               // each file is reported once
    fs->files["/rec/new.mp4"].data += "again";
    fs->files["/rec/new.mp4"].mtime = now;
    w.poll();
    now += 31;
    fs->files["/rec/new.mp4"].mtime = now - 40;
    fs->locked.insert("/rec/new.mp4");
    w.poll();
    EXPECT_EQ(got.size(), 2u);               // locked by the recorder: not reported
}

TEST(CloudSettings, SanitizeRoundTripAndDefaults) {
    CloudSettings s;
    EXPECT_FALSE(s.enabled);                  // off until the user turns it on
    EXPECT_FALSE(s.deleteAfterUpload);        // delete-after-upload is OFF by default
    EXPECT_TRUE(s.pauseWhileStreaming);
    s.s3Bucket = std::string(200, 'x');
    s.partSizeMiB = 1;
    s.stableSeconds = 1;
    s.bandwidthMbps = -5;
    s.provider = "ftp";
    s = sanitize(s);
    EXPECT_EQ(s.s3Bucket.size(), 63u);
    EXPECT_EQ(s.partSizeMiB, 5);
    EXPECT_EQ(s.stableSeconds, 10);
    EXPECT_EQ(s.bandwidthMbps, 0);
    EXPECT_EQ(s.provider, "s3");
    CloudSettings t;
    ASSERT_TRUE(settingsFromJson(settingsToJson(s), t));
    EXPECT_EQ(settingsToJson(t), settingsToJson(s));
    EXPECT_FALSE(settingsFromJson("[1,2", t));
    EXPECT_EQ(maskHint("AKIA1234ABCD"), "••••ABCD");
    EXPECT_EQ(maskHint(""), "");
    // credentials are not part of the settings file at all
    EXPECT_EQ(settingsToJson(s).find("secret"), std::string::npos);
}

namespace {
struct SvcRig {
    std::string dir;
    std::shared_ptr<FakeHttp> http = std::make_shared<FakeHttp>();
    std::shared_ptr<MemorySecretStore> secrets = std::make_shared<MemorySecretStore>();
    std::shared_ptr<MemFs> fs = std::make_shared<MemFs>();
    hb::core::EventBus bus;
    std::int64_t wall = 1'700'000'000;
    bool streaming = false;
    std::unique_ptr<CloudService> svc;
    explicit SvcRig(const char* tag) : dir(tempDir(tag)) { build(); }
    ~SvcRig() { svc.reset(); cleanup(dir); }
    void build() {
        CloudServiceDeps d;
        d.dataDir = dir;
        d.http = http;
        d.secrets = secrets;
        d.fs = fs;
        d.bus = &bus;
        d.recordingsFolder = [] { return std::string("/rec"); };
        d.streamingActive = [this] { return streaming; };
        d.wallNow = [this] { return wall; };
        d.startThreads = false;
        svc = std::make_unique<CloudService>(d);
    }
};
const char* kSaveJson =
    R"({"enabled":true,"provider":"s3","s3Endpoint":"https://s3.us-east-005.backblazeb2.com","s3Region":"us-east-005","s3Bucket":"meu-bucket",)"
    R"("s3AccessKey":"AKIAVERYSECRETID","s3SecretKey":"TOP-SECRET-VALUE-123","bandwidthMbps":4,"upIsoAudio":true})";
}  // namespace

TEST(CloudService, SecretsAreStoredSeparatelyAndNeverEchoedToTheUi) {
    SvcRig r("secrets");
    EXPECT_FALSE(r.svc->handle({"cloudsave", kSaveJson}).empty());
    const std::string st = r.svc->stateJson();
    EXPECT_EQ(st.find("TOP-SECRET-VALUE-123"), std::string::npos);
    EXPECT_EQ(st.find("AKIAVERYSECRETID"), std::string::npos);
    EXPECT_NE(st.find("••••ETID"), std::string::npos);   // only a masked hint of the access key id
    EXPECT_NE(st.find("\"s3Secret\":\"••••••••\""), std::string::npos);   // secret key: no characters at all
    EXPECT_NE(st.find("\"bandwidthMbps\":4"), std::string::npos);
    EXPECT_EQ(*r.secrets->get(kSecS3Secret), "TOP-SECRET-VALUE-123");
    // the on-disk settings file holds no credentials
    std::ifstream f(std::filesystem::path(r.dir) / "cloud.json");
    std::stringstream ss;
    ss << f.rdbuf();
    EXPECT_EQ(ss.str().find("TOP-SECRET"), std::string::npos);
    EXPECT_EQ(ss.str().find("AKIAVERY"), std::string::npos);
    // saving again with blank secrets keeps the stored ones
    r.svc->handle({"cloudsave", R"({"s3Bucket":"outro-bucket","s3AccessKey":"","s3SecretKey":""})"});
    EXPECT_EQ(*r.secrets->get(kSecS3Secret), "TOP-SECRET-VALUE-123");
    EXPECT_EQ(r.svc->settings().s3Bucket, "outro-bucket");
    r.svc->handle({"cloudclear", "s3"});
    EXPECT_FALSE(r.secrets->get(kSecS3Secret).has_value());
    EXPECT_NE(r.svc->stateJson().find("\"s3Secret\":\"\""), std::string::npos);
}

TEST(CloudService, SettingsPersistAcrossRestart) {
    SvcRig r("persist");
    r.svc->handle({"cloudsave", kSaveJson});
    r.build();
    EXPECT_TRUE(r.svc->settings().enabled);
    EXPECT_EQ(r.svc->settings().s3Bucket, "meu-bucket");
    EXPECT_EQ(r.svc->settings().bandwidthMbps, 4);
}

TEST(CloudService, ClosedFileEventsAreQueuedAccordingToWhatToUpload) {
    SvcRig r("events");
    r.fs->files["/rec/p.mp4"] = {"0123", r.wall - 100};
    r.fs->files["/rec/iso1.mp4"] = {"0123", r.wall - 100};
    r.fs->files["/rec/iso1.wav"] = {"0123", r.wall - 100};
    auto fire = [&](const char* path, const char* kind) { r.bus.publish({"recording.fileClosed", {{"path", path}, {"kind", kind}, {"index", "1"}}}); };
    fire("/rec/p.mp4", "program");
    EXPECT_TRUE(r.svc->engine().jobs().empty());   // master switch is OFF: nothing is queued
    r.svc->handle({"cloudsave", kSaveJson});       // program on, iso-video off, iso-audio on
    fire("/rec/p.mp4", "program");
    fire("/rec/iso1.mp4", "iso-video");
    fire("/rec/iso1.wav", "iso-audio");
    const auto jobs = r.svc->engine().jobs();
    ASSERT_EQ(jobs.size(), 2u);
    EXPECT_EQ(jobs[0].kind, "program");
    EXPECT_EQ(jobs[1].kind, "iso-audio");
    EXPECT_TRUE(jobs[0].closedByProducer);
}

TEST(CloudService, TestConnectionRunsAsyncAndReportsResult) {
    SvcRig r("test");
    r.http->handler = [](const HttpRequest& q) -> Result<HttpResponse> {
        if (q.method == "PUT") return resp(200, "", {{"ETag", "\"" + md5Hex(q.body) + "\""}});
        return resp(204);
    };
    EXPECT_NE(r.svc->stateJson().find("\"state\":\"idle\""), std::string::npos);
    r.svc->handle({"cloudsave", kSaveJson});
    r.svc->handle({"cloudtest"});
    r.svc->joinAsync();
    const std::string st = r.svc->stateJson();
    EXPECT_NE(st.find("\"test\":{\"state\":\"ok\""), std::string::npos) << st;
    // incomplete configuration is reported, not crashed on
    SvcRig r2("test2");
    r2.svc->handle({"cloudtest"});
    r2.svc->joinAsync();
    EXPECT_NE(r2.svc->stateJson().find("\"test\":{\"state\":\"error\""), std::string::npos);
}

TEST(CloudService, StateJsonListsJobsWithProgressAndSurvivesHostileNames) {
    SvcRig r("jobs");
    r.svc->handle({"cloudsave", kSaveJson});
    r.fs->files["/rec/we\"ird.mp4"] = {"0123", r.wall - 100};
    r.bus.publish({"recording.fileClosed", {{"path", "/rec/we\"ird.mp4"}, {"kind", "program"}}});
    const std::string st = r.svc->stateJson();
    EXPECT_NE(st.find("we\\\"ird.mp4"), std::string::npos);
    hb::core::json::Value v;
    EXPECT_TRUE(hb::core::json::parse(st, v));   // always valid JSON
    r.svc->handle({"cloudcancel", r.svc->engine().jobs()[0].id});
    EXPECT_EQ(r.svc->stateJson().find("we\\\"ird"), std::string::npos);   // cancelled jobs are hidden
}

#ifndef _WIN32
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
TEST(CloudLoopback, CatchesTheRedirectOnLoopbackOnly) {
    auto l = makeLoopbackListener();
    const int port = l->start();
    ASSERT_GT(port, 0);
    std::string line;
    std::thread client([&] {
        int s = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(static_cast<uint16_t>(port));
        ::inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
        if (::connect(s, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0) {
            const std::string req = "GET /?code=abc&state=xyz HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
            ::send(s, req.data(), req.size(), 0);
            char buf[512];
            ::recv(s, buf, sizeof buf, 0);
        }
        ::close(s);
    });
    EXPECT_TRUE(l->waitRequestLine(line, 5000, nullptr));
    EXPECT_EQ(line, "GET /?code=abc&state=xyz HTTP/1.1");
    l->respond(200, "<p>ok</p>");
    client.join();
    // timeout path returns false instead of hanging
    std::string none;
    EXPECT_FALSE(l->waitRequestLine(none, 300, nullptr));
    l->close();
}
#endif

TEST(CloudService, GoogleLoginFlowWithFakeBrowserAndTransport) {
#ifdef _WIN32
    GTEST_SKIP() << "loopback socket test runs on POSIX only";
#else
    SvcRig r("login");
    r.svc.reset();
    CloudServiceDeps d;
    d.dataDir = r.dir;
    d.http = r.http;
    d.secrets = r.secrets;
    d.fs = r.fs;
    d.startThreads = false;
    std::string openedUrl;
    d.openBrowser = [&](const std::string& url) {
        openedUrl = url;
        // play the user's browser: extract port + state and hit the loopback redirect
        const auto pp = url.find("127.0.0.1%3A");
        const int port = std::atoi(url.c_str() + pp + 12);
        const auto sp = url.find("&state=") + 7;
        const std::string state = url.substr(sp, url.find('&', sp) - sp);
        std::thread([port, state] {
            int s = ::socket(AF_INET, SOCK_STREAM, 0);
            sockaddr_in a{};
            a.sin_family = AF_INET;
            a.sin_port = htons(static_cast<uint16_t>(port));
            ::inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
            if (::connect(s, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0) {
                const std::string req = "GET /?state=" + state + "&code=AUTHCODE HTTP/1.1\r\n\r\n";
                ::send(s, req.data(), req.size(), 0);
                char buf[1024];
                ::recv(s, buf, sizeof buf, 0);
            }
            ::close(s);
        }).detach();
    };
    r.http->handler = [](const HttpRequest& q) -> Result<HttpResponse> {
        EXPECT_NE(q.body.find("code=AUTHCODE"), std::string::npos);
        return resp(200, R"({"access_token":"AT","refresh_token":"REFRESH-TOKEN-VALUE","expires_in":3600})");
    };
    CloudService svc(d);
    svc.handle({"cloudsave", R"({"provider":"gdrive","gClientId":"cid.apps.googleusercontent.com","gClientSecret":"gsec"})"});
    svc.handle({"cloudgoogle", "login"});
    svc.joinAsync();
    EXPECT_NE(openedUrl.find("accounts.google.com"), std::string::npos);
    const std::string st = svc.stateJson();
    EXPECT_NE(st.find("\"auth\":{\"state\":\"ok\""), std::string::npos) << st;
    EXPECT_EQ(*r.secrets->get(kSecGRefresh), "REFRESH-TOKEN-VALUE");
    EXPECT_EQ(st.find("REFRESH-TOKEN-VALUE"), std::string::npos);   // never sent to the UI
    EXPECT_NE(st.find("\"gConnected\":true"), std::string::npos);
#endif
}
