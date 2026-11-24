#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>

#include "core/json.hpp"
#include "multitrack/iso_session.hpp"
#include "recording/wav_writer.hpp"
#include "recording_test_util.hpp"

using namespace hb::multitrack;
namespace fs = std::filesystem;

namespace {

struct FakeLog {
    std::mutex mu;
    struct File {
        std::vector<std::int64_t> videoPts;
        std::uint64_t audioFrames = 0;
        bool closed = false;
    };
    std::map<std::string, File> files;
    std::atomic<int> failWriteOnVideoIndex{-1};   // fail the Nth video write of any path containing failTag
    std::string failTag;
};

class FakeWriter : public hb::rec::IMediaWriter {
public:
    explicit FakeWriter(FakeLog* l) : log_(l) {}
    bool open(const std::string& path, const hb::rec::RecordingProfile&, const hb::rec::VideoFormat&,
              const hb::rec::AudioFormat&, std::string&) override {
        path_ = path;
        std::ofstream(hb::rec::detail::pathFromUtf8(path)) << "x";
        std::lock_guard lk(log_->mu);
        log_->files[path];
        return true;
    }
    bool writeVideo(const std::uint8_t*, int, std::int64_t pts, std::string& err) override {
        std::lock_guard lk(log_->mu);
        if (!log_->failTag.empty() && path_.find(log_->failTag) != std::string::npos &&
            static_cast<int>(log_->files[path_].videoPts.size()) >= log_->failWriteOnVideoIndex) {
            err = "disco removido";
            return false;
        }
        log_->files[path_].videoPts.push_back(pts);
        ++stats_.videoFrames;
        return true;
    }
    bool writeAudio(const float*, std::size_t frames, std::int64_t, std::string&) override {
        std::lock_guard lk(log_->mu);
        log_->files[path_].audioFrames += frames;
        stats_.audioFrames += frames;
        return true;
    }
    bool close(std::string&) override {
        std::lock_guard lk(log_->mu);
        log_->files[path_].closed = true;
        return true;
    }
    hb::rec::WriterStats stats() const override { return stats_; }

private:
    FakeLog* log_;
    std::string path_;
    hb::rec::WriterStats stats_;
};

struct Fixture {
    hb::rectest::TempDir dir{"mt"};
    FakeLog log;
    hb::core::EventBus bus;
    std::vector<hb::core::Event> events;
    std::mutex evMu;
    std::atomic<std::int64_t> nowNs{1000000000};   // fake monotonic clock
    std::atomic<std::uint64_t> freeBytes{100ull << 30};

    Fixture() {
        bus.subscribe("recording.fileClosed", [this](const hb::core::Event& e) {
            std::lock_guard lk(evMu);
            events.push_back(e);
        });
    }
    IsoSessionConfig config(int cams, int chans) {
        IsoSessionConfig c;
        c.sessionDir = dir.utf8("session");
        c.sessionName = "t";
        c.bus = &bus;
        c.nowNs = [this] { return nowNs.load(); };
        c.wallMs = [] { return std::int64_t(1795529133250ll); };
        c.freeSpace = [this](const std::string&) { return freeBytes.load(); };
        c.autoSupervise = false;
        c.startSamplers = false;
        c.videoQueueDepth = 1000;   // tests push faster than real time
        c.findProfile = [this](const std::string& id, hb::rec::RecordingProfile& p, hb::rec::WriterFactory& f) {
            p.id = id;
            p.extension = id.rfind("mp4", 0) == 0 ? "mp4" : "wav";
            f = [this] { return std::unique_ptr<hb::rec::IMediaWriter>(new FakeWriter(&log)); };
            return true;
        };
        for (int i = 0; i < cams; ++i) {
            IsoVideoSpec v;
            v.slot = i;
            v.name = "Camera " + std::to_string(i + 1);
            v.width = 64; v.height = 36; v.fpsNum = 30; v.fpsDen = 1;
            c.video.push_back(v);
        }
        for (int i = 0; i < chans; ++i) {
            IsoAudioSpec a;
            a.strip = i;
            a.name = "MIC " + std::to_string(i + 1);
            c.audio.push_back(a);
        }
        return c;
    }
    static std::shared_ptr<const hb::video::Frame> frame() {
        auto f = std::make_shared<hb::video::Frame>();
        f->width = 64; f->height = 36; f->stride = 256;
        f->data.assign(256 * 36, 0);
        return f;
    }
};

}  // namespace

TEST(MtSession, WritesLayoutManifestAndAnnouncesFiles) {
    Fixture fx;
    IsoSession s(fx.config(1, 1));
    std::string err;
    ASSERT_TRUE(s.start(err)) << err;
    const std::vector<float> blk(480 * 2, 0.25f);
    for (int i = 0; i < 90; ++i) {                              // 3 s of video at 30 fps, one slot skipped at 40
        if (i != 40) s.pushVideo(0, Fixture::frame(), unitsToNs(i, 30, 1));
        s.pushAudio(0, blk.data(), 480, unitsToNs(std::uint64_t(i) * 480 / 16, 30, 1) * 0 + unitsToNs(std::uint64_t(i) * 480, 48000, 1));
    }
    s.stop();

    EXPECT_TRUE(fs::exists(fx.dir.path / "session" / "session.json"));
    EXPECT_TRUE(fs::exists(fx.dir.path / "session" / "iso"));
    EXPECT_TRUE(fs::exists(fx.dir.path / "session" / "audio"));
    EXPECT_TRUE(fs::exists(fx.dir.path / "session" / "edl" / "CAM01.edl"));

    std::ifstream f(fx.dir.path / "session" / "session.json", std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    f.close();
    hb::core::json::Value root;
    ASSERT_TRUE(hb::core::json::parse(text, root)) << text;
    EXPECT_EQ(root.get("state")->s, "finished");
    const auto& tr = root.get("tracks")->a;
    ASSERT_EQ(tr.size(), 2u);
    EXPECT_EQ(tr[0].get("id")->s, "CAM01");
    EXPECT_EQ(tr[0].get("units")->n, 90);               // 89 frames + 1 repeated slot
    EXPECT_EQ(tr[0].get("repeated")->n, 1);
    EXPECT_EQ(tr[1].get("id")->s, "CH01");
    EXPECT_EQ(tr[1].get("units")->n, 90 * 480);
    EXPECT_EQ(tr[1].get("tap")->s, "pre-fader");
    EXPECT_EQ(tr[0].get("segments")->a[0].get("file")->s, "iso/CAM01_Camera 1.mp4");

    std::lock_guard lk(fx.evMu);
    ASSERT_EQ(fx.events.size(), 2u);
    std::map<std::string, std::string> kinds;
    for (const auto& e : fx.events) kinds[e.fields.at("kind")] = e.fields.at("index");
    EXPECT_EQ(kinds.count("iso-video"), 1u);
    EXPECT_EQ(kinds.count("iso-audio"), 1u);
    EXPECT_EQ(kinds["iso-video"], "1");
}

TEST(MtSession, VideoPtsFollowTheSessionClockWithRepeatsForGaps) {
    Fixture fx;
    IsoSession s(fx.config(1, 0));
    std::string err;
    ASSERT_TRUE(s.start(err));
    s.pushVideo(0, Fixture::frame(), unitsToNs(3, 30, 1));      // first frame at slot 3 (offset 100 ms)
    s.pushVideo(0, Fixture::frame(), unitsToNs(4, 30, 1));
    s.pushVideo(0, Fixture::frame(), unitsToNs(7, 30, 1));      // slots 5, 6 repeated
    s.stop();
    std::lock_guard lk(fx.log.mu);
    ASSERT_EQ(fx.log.files.size(), 1u);
    const auto& pts = fx.log.files.begin()->second.videoPts;
    ASSERT_EQ(pts.size(), 5u);
    for (std::size_t i = 0; i < pts.size(); ++i) EXPECT_EQ(pts[i], unitsToNs(i, 30, 1));   // file starts at 0
    const SessionManifest m = s.manifest("finished");
    EXPECT_EQ(m.tracks[0].startOffsetNs, unitsToNs(3, 30, 1));
}

TEST(MtSession, AudioStartsOnTheSessionZeroWithLeadingSilence) {
    Fixture fx;
    IsoSession s(fx.config(0, 1));
    std::string err;
    ASSERT_TRUE(s.start(err));
    const std::vector<float> blk(480 * 2, 0.5f);
    s.pushAudio(0, blk.data(), 480, 20 * kNsPerMs);             // first audio 20 ms late: 960 frames of silence
    s.pushAudio(0, blk.data(), 480, 30 * kNsPerMs);
    s.stop();
    const SessionManifest m = s.manifest("finished");
    EXPECT_EQ(m.tracks[0].leadingSilenceFrames, 960u);
    EXPECT_EQ(m.tracks[0].units, 960u + 960u);
    EXPECT_EQ(m.tracks[0].startOffsetNs, 0);
}

TEST(MtSession, FailingTrackDoesNotKillTheOthers) {
    Fixture fx;
    fx.log.failTag = "CAM01";
    fx.log.failWriteOnVideoIndex = 3;
    IsoSession s(fx.config(2, 1));
    std::string err;
    ASSERT_TRUE(s.start(err));
    const std::vector<float> blk(480 * 2, 0.1f);
    for (int i = 0; i < 30; ++i) {
        s.pushVideo(0, Fixture::frame(), unitsToNs(i, 30, 1));
        s.pushVideo(1, Fixture::frame(), unitsToNs(i, 30, 1));
        s.pushAudio(0, blk.data(), 480, unitsToNs(std::uint64_t(i) * 480, 48000, 1));
        // let the worker of the failing track run before the next push so the failure is hit deterministically
    }
    s.stop();
    const auto st = s.status();
    ASSERT_EQ(st.tracks.size(), 3u);
    EXPECT_EQ(st.tracks[0].state, TrackState::Error);
    EXPECT_NE(st.tracks[0].error.find("disco"), std::string::npos);
    EXPECT_EQ(st.tracks[1].state, TrackState::Finished);
    EXPECT_EQ(st.tracks[1].units, 30u);
    EXPECT_EQ(st.tracks[2].state, TrackState::Finished);
    EXPECT_EQ(st.tracks[2].units, 30u * 480u);
}

TEST(MtSession, TrackThatCannotOpenIsReportedOthersRun) {
    Fixture fx;
    auto cfg = fx.config(1, 1);
    auto inner = cfg.findProfile;
    cfg.findProfile = [inner](const std::string& id, hb::rec::RecordingProfile& p, hb::rec::WriterFactory& f) {
        if (id == "wav") return false;   // audio format unavailable
        return inner(id, p, f);
    };
    IsoSession s(cfg);
    std::string err;
    ASSERT_TRUE(s.start(err));
    s.stop();
    const auto st = s.status();
    ASSERT_EQ(st.tracks.size(), 1u);
    EXPECT_FALSE(st.warnings.empty());
    EXPECT_NE(st.warnings[0].find("CH01"), std::string::npos);
}

TEST(MtSession, AllTracksFailingFailsTheStart) {
    Fixture fx;
    auto cfg = fx.config(0, 1);
    cfg.findProfile = [](const std::string&, hb::rec::RecordingProfile&, hb::rec::WriterFactory&) { return false; };
    IsoSession s(cfg);
    std::string err;
    EXPECT_FALSE(s.start(err));
    EXPECT_FALSE(err.empty());
}

TEST(MtSession, LowDiskStopsIsoTracksButReportsIt) {
    Fixture fx;
    IsoSession s(fx.config(1, 1));
    std::string err;
    ASSERT_TRUE(s.start(err));
    s.pushVideo(0, Fixture::frame(), 0);
    fx.freeBytes = 1ull << 30;           // below the 2 GiB ISO threshold
    s.superviseOnce();
    const auto st = s.status();
    ASSERT_EQ(st.tracks.size(), 2u);
    EXPECT_EQ(st.tracks[0].state, TrackState::DiskStopped);
    EXPECT_EQ(st.tracks[1].state, TrackState::DiskStopped);
    EXPECT_FALSE(st.warnings.empty());
    {
        std::lock_guard lk(fx.evMu);
        EXPECT_EQ(fx.events.size(), 2u);   // both files were finalised and announced
    }
    s.stop();
}

TEST(MtSession, RotationSplitsEveryFileAtTheSameBoundary) {
    Fixture fx;
    auto cfg = fx.config(1, 1);
    cfg.rotationSeconds = 1;
    IsoSession s(cfg);
    std::string err;
    ASSERT_TRUE(s.start(err));
    const std::vector<float> blk(480 * 2, 0.1f);
    for (int i = 0; i < 75; ++i) {      // 2.5 s of video, 2.5 s of audio
        s.pushVideo(0, Fixture::frame(), unitsToNs(i, 30, 1));
        if (i % 3 == 0) s.pushAudio(0, blk.data(), 480, unitsToNs(std::uint64_t(i / 3) * 480, 48000, 1));
    }
    for (int k = 25; k < 250; ++k) s.pushAudio(0, blk.data(), 480, unitsToNs(std::uint64_t(k) * 480, 48000, 1));
    s.stop();
    const SessionManifest m = s.manifest("finished");
    ASSERT_EQ(m.tracks.size(), 2u);
    const auto& v = m.tracks[0].segments;
    ASSERT_EQ(v.size(), 3u);
    EXPECT_EQ(v[0].file, "iso/CAM01_Camera 1_001.mp4");
    EXPECT_EQ(v[1].file, "iso/CAM01_Camera 1_002.mp4");
    EXPECT_EQ(v[0].units, 30u);
    EXPECT_EQ(v[1].startUnit, 30u);
    EXPECT_EQ(v[1].units, 30u);
    EXPECT_EQ(v[2].units, 15u);
    const auto& a = m.tracks[1].segments;
    ASSERT_GE(a.size(), 3u);
    EXPECT_EQ(a[0].units, 48000u);
    EXPECT_EQ(a[1].startNs, kNsPerSec);
    for (const auto& seg : a) EXPECT_TRUE(seg.closed);
    std::lock_guard lk(fx.evMu);
    EXPECT_EQ(fx.events.size(), v.size() + a.size());   // every rotated file announced
}

TEST(MtSession, SlowEncoderDropsOldestFramesAndSaysSo) {
    Fixture fx;
    auto cfg = fx.config(1, 0);
    cfg.videoQueueDepth = 6;
    std::atomic<bool> release{false};
    std::atomic<int> written{0};
    struct Slow : FakeWriter {
        Slow(FakeLog* l, std::atomic<bool>* r, std::atomic<int>* w) : FakeWriter(l), rel(r), wr(w) {}
        bool writeVideo(const std::uint8_t* d, int s, std::int64_t p, std::string& e) override {
            while (!rel->load()) std::this_thread::sleep_for(std::chrono::milliseconds(20));
            ++*wr;
            return FakeWriter::writeVideo(d, s, p, e);
        }
        std::atomic<bool>* rel;
        std::atomic<int>* wr;
    };
    cfg.findProfile = [&](const std::string& id, hb::rec::RecordingProfile& p, hb::rec::WriterFactory& f) {
        p.id = id;
        p.extension = "mp4";
        f = [&] { return std::unique_ptr<hb::rec::IMediaWriter>(new Slow(&fx.log, &release, &written)); };
        return true;
    };
    IsoSession s(cfg);
    std::string err;
    ASSERT_TRUE(s.start(err));
    for (int i = 0; i < 40; ++i) s.pushVideo(0, Fixture::frame(), unitsToNs(i, 30, 1));   // never blocks
    auto st = s.status();
    EXPECT_GE(st.tracks[0].droppedQueue, 40u - 6u - 1u);
    release = true;
    s.stop();
    st = s.status();
    EXPECT_EQ(st.tracks[0].state, TrackState::Finished);
    EXPECT_GT(st.tracks[0].droppedQueue, 0u);
}

TEST(MtSession, WavHeaderIsRecoverableBeforeClose) {
    // Real WavWriter: after the periodic header sync the on-disk file already declares the right sizes,
    // so a crash (no close()) loses at most the last sync interval.
    hb::rectest::TempDir dir("wavsync");
    hb::rec::WavWriter w;
    hb::rec::RecordingProfile p;
    p.audioBitsPerSample = 24;
    std::string err;
    ASSERT_TRUE(w.open(dir.utf8("a.wav"), p, {}, {48000, 2}, err)) << err;
    const std::vector<float> blk(4800 * 2, 0.5f);
    for (int i = 0; i < 10; ++i) ASSERT_TRUE(w.writeAudio(blk.data(), 4800, 0, err));
    ASSERT_TRUE(w.syncHeader(err)) << err;
    // read the file while the writer is still open
    const auto bytes = hb::rectest::readFile(dir.path / "a.wav");
    ASSERT_GT(bytes.size(), 44u);
    const auto info = hb::rectest::parseWav(bytes);
    EXPECT_TRUE(info.ok) << info.error;
    EXPECT_EQ(info.frames, 48000u);
    ASSERT_TRUE(w.close(err));
    const auto fin = hb::rectest::parseWav(hb::rectest::readFile(dir.path / "a.wav"));
    EXPECT_TRUE(fin.ok);
    EXPECT_EQ(fin.frames, 48000u);
}
