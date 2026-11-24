#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "cloud/hash_util.hpp"
#include "cloud/throttle.hpp"
#include "cloud/upload_engine.hpp"
#include "cloud_test_util.hpp"

using namespace hb::cloud;
using namespace cloudtest;

namespace {

struct Rig {
    std::shared_ptr<MemFs> fs = std::make_shared<MemFs>();
    std::shared_ptr<FakeProvider> prov = std::make_shared<FakeProvider>();
    std::int64_t wall = 1'700'000'000;
    double mono = 0;
    bool streaming = false;
    std::vector<double> sleeps;
    std::unique_ptr<UploadEngine> eng;

    explicit Rig(const std::string& journal = "") { build(journal); }
    void build(const std::string& journal) {
        EngineOptions o;
        o.journalPath = journal;
        o.fs = fs;
        o.wallNow = [this] { return wall; };
        o.monoNow = [this] { return mono; };
        o.sleepFn = [this](double s) { sleeps.push_back(s); mono += s; };
        o.streamingActive = [this] { return streaming; };
        o.stableSeconds = 30;
        eng = std::make_unique<UploadEngine>(o);
        eng->setProvider(prov);
    }
    void put(const std::string& path, const std::string& data, std::int64_t age = 3600) { fs->files[path] = {data, wall - age}; }
    void drain(int max = 200) {
        for (int i = 0; i < max && eng->runOnce(); ++i) {}
    }
    Job job(std::size_t i = 0) { return eng->jobs().at(i); }
};

std::string tempJournal(const char* tag) {
    auto p = std::filesystem::temp_directory_path() / (std::string("hb_cloud_") + tag + "_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + ".json");
    std::error_code ec;
    std::filesystem::remove(p, ec);
    return p.string();
}

}  // namespace

TEST(CloudThrottle, TokenBucketHonoursRateWithInjectedClock) {
    double now = 0;
    TokenBucket b([&] { return now; });
    b.setRate(1000, 1000);                 // 1000 B/s, burst 1000
    EXPECT_EQ(b.tryTake(1000), 0);         // starts full
    EXPECT_NEAR(b.tryTake(500), 0.5, 1e-9);  // empty: need 0.5 s
    now += 0.5;
    EXPECT_EQ(b.tryTake(500), 0);
    now += 100;                            // refill is capped at the burst
    EXPECT_EQ(b.tryTake(1000), 0);
    EXPECT_GT(b.tryTake(1), 0);
    EXPECT_EQ(b.tryTake(1e9) > 0, true);   // oversize requests are clamped to the burst, still limited
    b.setRate(0);
    EXPECT_EQ(b.tryTake(1e9), 0);          // unlimited
    EXPECT_DOUBLE_EQ(mbpsToBytesPerSec(8), 1'000'000);
}

TEST(CloudEngine, UploadsInChunksAndFinishes) {
    Rig r;
    r.put("/rec/a.mp4", "0123456789");  // 10 bytes, chunk 4 -> 3 chunks
    EXPECT_EQ(r.eng->enqueue("/rec/a.mp4", "program", 0, true), UploadEngine::Enqueue::Added);
    r.drain();
    const Job j = r.job();
    EXPECT_EQ(j.state, JobState::Done);
    EXPECT_EQ(r.prov->puts, 3);
    ASSERT_EQ(r.prov->objects.size(), 1u);
    EXPECT_EQ(r.prov->objects.begin()->second, "0123456789");
    EXPECT_NE(j.remoteName.find("/program/a.mp4"), std::string::npos);
    EXPECT_EQ(r.fs->files.count("/rec/a.mp4"), 1u);  // local file kept: delete-after-upload is OFF by default
    EXPECT_EQ(r.eng->totals().done, 1);
}

TEST(CloudEngine, RemoteNamesAreDatedAndSafe) {
    EXPECT_EQ(UploadEngine::remoteNameFor("C:\\Videos\\HB Studio\\cul:to?.mp4", "program", 1440938160), "2015-08-30/program/cul_to_.mp4");
    EXPECT_EQ(UploadEngine::remoteNameFor("/x/y.wav", "weird", 0), "1970-01-01/outros/y.wav");
}

TEST(CloudEngine, RejectsWrongExtensionsMissingAndEmptyFiles) {
    Rig r;
    r.put("/rec/a.txt", "x");
    r.put("/rec/e.mp4", "");
    std::string why;
    EXPECT_EQ(r.eng->enqueue("/rec/a.txt", "program", 0, true, &why), UploadEngine::Enqueue::Rejected);
    EXPECT_EQ(r.eng->enqueue("/rec/none.mp4", "program", 0, true, &why), UploadEngine::Enqueue::Rejected);
    EXPECT_EQ(r.eng->enqueue("/rec/e.mp4", "program", 0, true, &why), UploadEngine::Enqueue::Rejected);
    EXPECT_TRUE(r.eng->jobs().empty());
}

TEST(CloudEngine, DuplicatesAreIgnoredAndDedupeAgainstRemote) {
    Rig r;
    r.put("/rec/a.mp4", "0123456789");
    EXPECT_EQ(r.eng->enqueue("/rec/a.mp4", "program", 0, true), UploadEngine::Enqueue::Added);
    EXPECT_EQ(r.eng->enqueue("/rec/a.mp4", "program", 0, true), UploadEngine::Enqueue::Duplicate);
    r.prov->objects[r.job().remoteName] = "0123456789";  // same name + same size already at the destination
    r.drain();
    EXPECT_EQ(r.job().state, JobState::Done);
    EXPECT_EQ(r.prov->puts, 0);
    EXPECT_NE(r.job().note.find("Já existia"), std::string::npos);
    EXPECT_EQ(r.eng->enqueue("/rec/a.mp4", "program", 0, true), UploadEngine::Enqueue::Duplicate);  // done jobs are remembered too
}

TEST(CloudEngine, NeverUploadsAFileThatMayStillBeWritten) {
    Rig r;
    r.put("/rec/live.mp4", "0123456789", /*age*/ 2);  // modified 2 s ago, producer did not say it is closed
    r.eng->enqueue("/rec/live.mp4", "program", 0, /*closed*/ false);
    r.drain();
    EXPECT_EQ(r.prov->puts, 0);
    EXPECT_EQ(r.job().state, JobState::Queued);
    r.wall += 31;  // quiet for > stableSeconds, still unchanged
    r.fs->files["/rec/live.mp4"].mtime = r.wall - 40;
    r.fs->locked.insert("/rec/live.mp4");  // ...but another process still has it open
    r.drain();
    EXPECT_EQ(r.prov->puts, 0);
    r.fs->locked.clear();
    r.wall += 31;
    r.drain();
    EXPECT_EQ(r.job().state, JobState::Done);
}

TEST(CloudEngine, GrowingFileRestartsInsteadOfUploadingAPartialRecording) {
    Rig r;
    r.put("/rec/g.mp4", "0123456789", 100);
    r.eng->enqueue("/rec/g.mp4", "program", 0, false);
    r.wall += 31;
    r.fs->files["/rec/g.mp4"].data += "MORE";       // grew after being queued
    r.fs->files["/rec/g.mp4"].mtime = r.wall - 1;
    r.drain();
    EXPECT_EQ(r.prov->puts, 0);
    EXPECT_NE(r.job().error.find("mudando"), std::string::npos);
    EXPECT_EQ(r.job().size, 14u);
}

TEST(CloudEngine, TransientFailuresBackOffExponentiallyThenSucceed) {
    Rig r;
    r.put("/rec/a.mp4", "01234567");  // 2 chunks
    r.eng->enqueue("/rec/a.mp4", "program", 0, true);
    r.prov->failNextPut = {makeError(ErrorKind::Transient, "rede"), makeError(ErrorKind::Transient, "rede"), makeError(ErrorKind::Transient, "rede")};
    EXPECT_TRUE(r.eng->runOnce());   // begin + failed put #1
    EXPECT_EQ(r.job().state, JobState::Queued);
    EXPECT_EQ(r.job().attempts, 1);
    EXPECT_EQ(r.job().nextAt, r.wall + 5);
    EXPECT_FALSE(r.eng->runOnce());  // not yet: nextAt in the future
    r.wall += 5;
    EXPECT_TRUE(r.eng->runOnce());
    EXPECT_EQ(r.job().nextAt, r.wall + 10);   // 5 -> 10
    r.wall += 10;
    EXPECT_TRUE(r.eng->runOnce());
    EXPECT_EQ(r.job().nextAt, r.wall + 20);   // -> 20
    r.wall += 20;
    r.drain();
    EXPECT_EQ(r.job().state, JobState::Done);
    EXPECT_EQ(r.job().attempts, 0);
    EXPECT_EQ(r.eng->backoffFor(30), 600);    // capped
}

TEST(CloudEngine, GivesUpAfterMaxAttemptsAndRetryRestarts) {
    Rig r;
    r.put("/rec/a.mp4", "01234567");
    r.eng->enqueue("/rec/a.mp4", "program", 0, true);
    r.prov->failNextPut.assign(20, makeError(ErrorKind::Transient, "rede"));
    for (int i = 0; i < 30; ++i) { r.wall += 700; r.eng->runOnce(); }
    EXPECT_EQ(r.job().state, JobState::Error);
    EXPECT_NE(r.job().error.find("8 tentativas"), std::string::npos);
    r.prov->failNextPut.clear();
    EXPECT_TRUE(r.eng->retry(r.job().id));
    r.drain();
    EXPECT_EQ(r.job().state, JobState::Done);
}

TEST(CloudEngine, QuotaAndAuthErrorsStopWithoutRetryStorm) {
    Rig r;
    r.put("/rec/a.mp4", "01234567");
    r.eng->enqueue("/rec/a.mp4", "program", 0, true);
    r.prov->failNextPut = {makeError(ErrorKind::Quota, "Cota do provedor atingida")};
    r.drain();
    EXPECT_EQ(r.job().state, JobState::Error);
    EXPECT_EQ(r.job().errorKind, "quota");
    EXPECT_EQ(r.job().error, "Cota do provedor atingida");
    const int puts = r.prov->puts;
    r.wall += 10000;
    r.drain();
    EXPECT_EQ(r.prov->puts, puts);  // no automatic retry
    EXPECT_EQ(r.eng->totals().error, 1);
    EXPECT_EQ(r.eng->retryAll(), 1);
}

TEST(CloudEngine, SessionLostRestartsTheFileFromZero) {
    Rig r;
    r.put("/rec/a.mp4", "01234567");
    r.eng->enqueue("/rec/a.mp4", "program", 0, true);
    EXPECT_TRUE(r.eng->runOnce());  // first chunk ok
    EXPECT_EQ(r.job().done, 4u);
    r.prov->failNextPut = {makeError(ErrorKind::SessionLost, "sessão expirou")};
    EXPECT_TRUE(r.eng->runOnce());
    EXPECT_EQ(r.job().done, 0u);
    EXPECT_FALSE(r.job().hasSession);
    r.wall += 5;
    r.drain();
    EXPECT_EQ(r.job().state, JobState::Done);
    EXPECT_EQ(r.prov->begins, 2);
    EXPECT_EQ(r.prov->objects.begin()->second, "01234567");
}

TEST(CloudEngine, MissingLocalFileIsALocalError) {
    Rig r;
    r.put("/rec/a.mp4", "01234567");
    r.eng->enqueue("/rec/a.mp4", "program", 0, true);
    r.fs->files.clear();
    r.drain();
    EXPECT_EQ(r.job().state, JobState::Error);
    EXPECT_EQ(r.job().errorKind, "local");
}

TEST(CloudEngine, PauseWhileStreamingAndKillSwitch) {
    Rig r;
    r.put("/rec/a.mp4", "01234567");
    r.eng->enqueue("/rec/a.mp4", "program", 0, true);
    r.streaming = true;
    r.eng->setPauseWhileStreaming(true);
    EXPECT_FALSE(r.eng->runOnce());
    EXPECT_EQ(r.prov->puts, 0);
    auto t = r.eng->totals();
    EXPECT_TRUE(t.paused);
    EXPECT_NE(t.pauseReason.find("transmissão"), std::string::npos);
    r.eng->setPauseWhileStreaming(false);   // option off: uploads even while streaming
    EXPECT_TRUE(r.eng->runOnce());
    r.eng->setPauseWhileStreaming(true);
    EXPECT_FALSE(r.eng->runOnce());
    r.streaming = false;
    r.eng->setEnabled(false);               // kill switch
    EXPECT_FALSE(r.eng->runOnce());
    EXPECT_NE(r.eng->totals().pauseReason.find("desligado"), std::string::npos);
    r.eng->setEnabled(true);
    r.drain();
    EXPECT_EQ(r.job().state, JobState::Done);
}

TEST(CloudEngine, StreamStartingMidChunkAbortsTheTransferWithoutPenalty) {
    Rig r;
    r.put("/rec/a.mp4", "01234567");
    r.eng->enqueue("/rec/a.mp4", "program", 0, true);
    r.prov->onPut = [&] { r.streaming = true; };  // the stream starts while a chunk is in flight
    EXPECT_TRUE(r.eng->runOnce());
    EXPECT_EQ(r.prov->objects.size(), 0u);
    EXPECT_EQ(r.job().state, JobState::Queued);
    EXPECT_EQ(r.job().attempts, 0);  // an intentional abort is not a failed attempt
    EXPECT_EQ(r.job().done, 0u);
    r.prov->onPut = nullptr;
    r.streaming = false;
    r.drain();
    EXPECT_EQ(r.job().state, JobState::Done);
}

TEST(CloudEngine, BandwidthCapThrottlesViaTheGate) {
    Rig r;
    r.put("/rec/a.mp4", std::string(300 * 1024, 'x'));
    r.prov->chunk = 300 * 1024;
    r.eng->enqueue("/rec/a.mp4", "program", 0, true);
    r.eng->setBandwidthMbps(0.8);   // 100 kB/s: the burst is 64 KiB, so a 300 KiB chunk must wait
    r.drain();
    EXPECT_EQ(r.job().state, JobState::Done);
    double slept = 0;
    for (double s : r.sleeps) slept += s;
    EXPECT_GT(slept, 1.5);          // ~ (300 KiB - burst) / 100 kB/s, on the injected clock (no real sleeping)
}

TEST(CloudEngine, CancelWhileQueuedAbortsRemoteSessionAndIsRemembered) {
    Rig r;
    r.put("/rec/a.mp4", "01234567");
    r.eng->enqueue("/rec/a.mp4", "program", 0, true);
    EXPECT_TRUE(r.eng->runOnce());
    EXPECT_TRUE(r.eng->cancel(r.job().id));
    EXPECT_EQ(r.job().state, JobState::Cancelled);
    EXPECT_EQ(r.prov->aborts, 1);
    r.drain();
    EXPECT_EQ(r.prov->objects.size(), 0u);
    EXPECT_EQ(r.eng->enqueue("/rec/a.mp4", "program", 0, true), UploadEngine::Enqueue::Duplicate);  // the folder watcher will not re-add it
    EXPECT_EQ(r.eng->clearFinished(), 1);
}

TEST(CloudEngine, DeleteAfterUploadOnlyWhenEnabledAndFileUnchanged) {
    Rig r;
    r.put("/rec/a.mp4", "01234567");
    r.eng->setDeleteAfterUpload(true);
    r.eng->enqueue("/rec/a.mp4", "program", 0, true);
    r.drain();
    EXPECT_EQ(r.job().state, JobState::Done);
    EXPECT_EQ(r.fs->files.count("/rec/a.mp4"), 0u);
    EXPECT_NE(r.job().note.find("apagado"), std::string::npos);
}

TEST(CloudEngine, WholeFileMd5ProviderGetsTheLocalMd5) {
    Rig r;
    r.prov->wholeMd5 = true;
    r.put("/rec/a.mp4", "01234567");
    r.eng->enqueue("/rec/a.mp4", "program", 0, true);
    r.drain();
    EXPECT_EQ(r.job().state, JobState::Done);
}

TEST(CloudEngine, JournalSurvivesRestartAndResumesFromTheServersOffset) {
    const std::string journal = tempJournal("resume");
    {
        Rig r(journal);
        r.put("/rec/a.mp4", "0123456789");
        r.eng->enqueue("/rec/a.mp4", "program", 0, true);
        EXPECT_TRUE(r.eng->runOnce());  // 4 of 10 bytes stored
        EXPECT_TRUE(r.eng->runOnce());  // 8 of 10
        EXPECT_EQ(r.job().done, 8u);
        // "app exits" here: provider keeps its partial state, journal is on disk
        auto fs = r.fs;
        auto prov = r.prov;
        Rig r2(journal);
        r2.fs = fs;
        r2.prov = prov;
        r2.build(journal);
        ASSERT_TRUE(r2.eng->loadJournal());
        ASSERT_EQ(r2.eng->jobs().size(), 1u);
        EXPECT_EQ(r2.job().state, JobState::Queued);     // "uploading" is demoted on load
        EXPECT_TRUE(r2.job().hasSession);
        r2.drain();
        EXPECT_EQ(r2.job().state, JobState::Done);
        EXPECT_EQ(prov->begins, 1);                      // no second session
        EXPECT_EQ(prov->commits, 1);                     // it asked the provider what is stored
        EXPECT_EQ(prov->objects.begin()->second, "0123456789");
        EXPECT_EQ(prov->puts, 3);                        // 2 before the restart + only the last chunk after
    }
    std::error_code ec;
    std::filesystem::remove(journal, ec);
}

TEST(CloudEngine, CorruptJournalIsIgnored) {
    const std::string journal = tempJournal("corrupt");
    {
        std::ofstream f(journal, std::ios::binary);
        f << "{not json";
    }
    Rig r(journal);
    EXPECT_FALSE(r.eng->loadJournal());
    EXPECT_TRUE(r.eng->jobs().empty());
    std::error_code ec;
    std::filesystem::remove(journal, ec);
}

TEST(CloudEngine, WorkerThreadUploadsInTheBackground) {
    auto fs = std::make_shared<MemFs>();
    auto prov = std::make_shared<FakeProvider>();
    EngineOptions o;
    o.fs = fs;
    std::atomic<bool> init{false};
    o.threadInit = [&] { init = true; };
    UploadEngine e(o);
    e.setProvider(prov);
    const std::int64_t now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    fs->files["/rec/a.mp4"] = {"01234567", now - 3600};
    e.start();
    e.enqueue("/rec/a.mp4", "program", 0, true);
    for (int i = 0; i < 400 && e.totals().done == 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    e.stop();
    EXPECT_TRUE(init);
    EXPECT_EQ(e.totals().done, 1);
}
