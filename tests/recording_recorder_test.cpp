#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "recording/file_io.hpp"
#include "recording/media_writer.hpp"
#include "recording/recorder.hpp"
#include "recording_test_util.hpp"

using namespace hb::rec;
using namespace hb::rectest;
using namespace std::chrono_literals;

namespace {

// ------------------------------------------------------------------ FakeWriter
struct Ev {
    enum Kind { Open, Video, Audio, Close } kind;
    int file = 0;  // 1-based index of the file (open count when the event happened)
    std::string path;
    std::int64_t pts = 0;
    std::size_t n = 0;
    const std::uint8_t* ptr = nullptr;
};

struct FakeState {
    std::mutex m;
    std::vector<Ev> ev;
    int opens = 0, closes = 0;
    int failOpenOn = 0;        // 1-based index of the open() that fails
    int failVideoAfter = -1;   // writeVideo fails once this many calls succeeded
    int failAudioAfter = -1;
    std::atomic<bool> gate{false};  // while true writeVideo blocks (simulates a stalled disk)
    std::atomic<bool> entered{false};
    std::uint64_t bytesPerVideo = 0, bytesPerAudioFrame = 0;
    int videoCalls = 0, audioCalls = 0;
    VideoFormat lastV;
    AudioFormat lastA;
    RecordingProfile lastP;

    std::vector<Ev> ofKind(Ev::Kind k, int file = 0) {
        std::lock_guard lk(m);
        std::vector<Ev> out;
        for (const auto& e : ev)
            if (e.kind == k && (file == 0 || e.file == file)) out.push_back(e);
        return out;
    }
    std::uint64_t audioFrames(int file = 0) {
        std::uint64_t t = 0;
        for (const auto& e : ofKind(Ev::Audio, file)) t += e.n;
        return t;
    }
};
std::shared_ptr<FakeState> gFake;

class FakeWriter : public IMediaWriter {
public:
    explicit FakeWriter(std::shared_ptr<FakeState> s) : s_(std::move(s)) {}
    bool open(const std::string& path, const RecordingProfile& p, const VideoFormat& v, const AudioFormat& a, std::string& error) override {
        std::lock_guard lk(s_->m);
        ++s_->opens;
        file_ = s_->opens;
        s_->lastV = v;
        s_->lastA = a;
        s_->lastP = p;
        if (s_->failOpenOn == s_->opens) {
            error = "falha injetada no open";
            return false;
        }
        std::ofstream(hb::rec::detail::pathFromUtf8(path)) << "fake";
        s_->ev.push_back({Ev::Open, file_, path, 0, 0, nullptr});
        open_ = true;
        return true;
    }
    bool writeVideo(const std::uint8_t* bgra, int, std::int64_t pts, std::string& error) override {
        s_->entered = true;
        while (s_->gate.load()) std::this_thread::sleep_for(1ms);
        std::lock_guard lk(s_->m);
        if (!open_ || dead_) return false;
        if (s_->failVideoAfter >= 0 && s_->videoCalls >= s_->failVideoAfter) {
            dead_ = true;
            error = "falha injetada no write de video";
            return false;
        }
        ++s_->videoCalls;
        ++st_.videoFrames;
        st_.bytesWritten += s_->bytesPerVideo;
        s_->ev.push_back({Ev::Video, file_, {}, pts, 0, bgra});
        return true;
    }
    bool writeAudio(const float*, std::size_t frames, std::int64_t pts, std::string& error) override {
        std::lock_guard lk(s_->m);
        if (!open_ || dead_) return false;
        if (s_->failAudioAfter >= 0 && s_->audioCalls >= s_->failAudioAfter) {
            dead_ = true;
            error = "falha injetada no write de audio";
            return false;
        }
        ++s_->audioCalls;
        st_.audioFrames += frames;
        st_.bytesWritten += frames * s_->bytesPerAudioFrame;
        s_->ev.push_back({Ev::Audio, file_, {}, pts, frames, nullptr});
        return true;
    }
    bool close(std::string&) override {
        std::lock_guard lk(s_->m);
        if (!open_) return true;
        open_ = false;
        ++s_->closes;
        s_->ev.push_back({Ev::Close, file_, {}, 0, 0, nullptr});
        return true;
    }
    WriterStats stats() const override { return st_; }

private:
    std::shared_ptr<FakeState> s_;
    int file_ = 0;
    bool open_ = false, dead_ = false;
    WriterStats st_;
};

void registerFakes() {
    static bool done = false;
    if (done) return;
    done = true;
    const auto factory = [] { return std::unique_ptr<IMediaWriter>(new FakeWriter(gFake)); };
    RecordingProfile av;
    av.id = "fake-av";
    av.label = "Fake A/V";
    av.extension = "fake";
    av.kind = "video";
    WriterRegistry::instance().add(av, factory);
    RecordingProfile au = av;
    au.id = "fake-audio";
    au.extension = "fka";
    au.hasVideo = false;
    au.kind = "audio";
    WriterRegistry::instance().add(au, factory);
    RecordingProfile vo = av;
    vo.id = "fake-video";
    vo.extension = "fkv";
    vo.hasAudio = false;
    WriterRegistry::instance().add(vo, factory);
}

template <class Pred>
bool waitFor(Pred p, int ms = 5000) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        if (p()) return true;
        std::this_thread::sleep_for(1ms);
    }
    return p();
}

struct Fixture : ::testing::Test {
    std::unique_ptr<TempDir> dir;
    hb::core::EventBus bus;
    std::vector<hb::core::Event> events;
    std::mutex evMu;
    std::shared_ptr<hb::core::MemorySink> sink = std::make_shared<hb::core::MemorySink>();
    hb::core::Logger logger;

    void SetUp() override {
        registerFakes();
        registerPortableWriters();
        gFake = std::make_shared<FakeState>();
        dir = std::make_unique<TempDir>("rec");
        logger.addSink(sink);
        bus.subscribe("recording.*", [this](const hb::core::Event& e) {
            std::lock_guard lk(evMu);
            events.push_back(e);
        });
    }
    void TearDown() override { gFake->gate = false; }

    std::vector<std::string> topics() {
        std::lock_guard lk(evMu);
        std::vector<std::string> t;
        for (const auto& e : events) t.push_back(e.topic);
        return t;
    }
    int count(const std::string& topic) {
        int n = 0;
        for (const auto& t : topics()) n += t == topic;
        return n;
    }

    RecorderConfig cfg(const std::string& profile = "fake-av") {
        RecorderConfig c;
        c.profileId = profile;
        c.folderUtf8 = dir->folderUtf8();
        c.video = VideoFormat{64, 36, 30, 1};
        c.audio = AudioFormat{48000, 2};
        c.minFreeBytes = 0;
        return c;
    }

    // Feeds `frames` video frames with their audio (1600 samples at 30 fps / 48 kHz), waiting for the
    // writer thread after every frame so that the bounded queues never overflow.
    void feedPaced(Recorder& r, int frames, int firstIndex = 0, bool audio = true) {
        auto f = makeFrame(64 + 2, 36, 0);
        auto small = std::make_shared<hb::video::Frame>();
        small->width = 64;
        small->height = 36;
        small->stride = 64 * 4;
        small->data.assign(static_cast<std::size_t>(small->stride) * 36, 128);
        std::vector<float> a(1600 * 2, 0.1f);
        const auto base = r.status().videoFrames;
        for (int i = 0; i < frames; ++i) {
            auto fr = std::make_shared<hb::video::Frame>(*small);
            fr->index = static_cast<std::uint64_t>(firstIndex + i);
            r.pushVideo(fr);
            if (audio) r.pushAudio(a.data(), 1600);
            if (!waitFor([&] {
                    const auto st = r.status();
                    return st.videoFrames >= base + static_cast<std::uint64_t>(i) + 1 || st.state != Recorder::Status::State::Recording;
                }))
                break;
            if (r.status().state != Recorder::Status::State::Recording) break;
        }
    }
};

}  // namespace

// ---------------------------------------------------------------- basic flow, A/V alignment
TEST_F(Fixture, AudioIsTheClockAndVideoIsConstantRate) {
    Recorder r(cfg(), &bus, &logger);
    std::string err;
    ASSERT_TRUE(r.start(err)) << err;
    EXPECT_EQ(r.status().state, Recorder::Status::State::Recording);
    EXPECT_EQ(r.status().segment, 1);
    // 0.1 s of audio BEFORE the first video frame: A/V alignment discards it (documented behaviour)
    std::vector<float> pre(4800 * 2, 0.3f);
    r.pushAudio(pre.data(), 4800);
    feedPaced(r, 90);
    r.stop();
    const auto s = r.status();
    EXPECT_EQ(s.state, Recorder::Status::State::Idle);
    EXPECT_EQ(s.videoFrames, 90u);
    EXPECT_EQ(s.repeatedVideo, 0u);
    EXPECT_EQ(s.droppedVideo, 0u);
    EXPECT_EQ(s.audioFrames, 90u * 1600);  // the pre-roll is not in the file
    EXPECT_EQ(s.droppedAudio, 0u);
    EXPECT_NEAR(s.seconds, 3.0, 1e-9);
    EXPECT_EQ(s.segment, 1);
    EXPECT_TRUE(s.error.empty());
    // writer saw: first video pts 0, constant 1/30 s steps; audio pts = samples so far
    const auto v = gFake->ofKind(Ev::Video);
    ASSERT_EQ(v.size(), 90u);
    for (std::size_t i = 0; i < v.size(); ++i) EXPECT_EQ(v[i].pts, static_cast<std::int64_t>(i) * 1000000000 / 30) << i;
    std::uint64_t cum = 0;
    for (const auto& a : gFake->ofKind(Ev::Audio)) {
        EXPECT_EQ(a.pts, static_cast<std::int64_t>(cum * 1000000000ull / 48000)) << cum;
        cum += a.n;
    }
    EXPECT_EQ(cum, 90u * 1600);
    // format handed to the writer
    EXPECT_EQ(gFake->lastV.width, 64);
    EXPECT_EQ(gFake->lastA.sampleRate, 48000);
    EXPECT_EQ(gFake->closes, 1);
    // FEATURE: multitrack - every finalised file is announced (recording.fileClosed) before "stopped".
    EXPECT_EQ(topics(), (std::vector<std::string>{"recording.started", "recording.fileClosed", "recording.stopped"}));
    // only the file name appears in the log, not the user's folder
    for (const auto& rec : sink->snapshot()) EXPECT_EQ(rec.message.find(dir->folderUtf8()), std::string::npos) << rec.message;
}

TEST_F(Fixture, NtscFrameRateTimestampsAreExact) {
    auto c = cfg();
    c.video.fpsNum = 30000;
    c.video.fpsDen = 1001;
    Recorder r(c, &bus, &logger);
    std::string err;
    ASSERT_TRUE(r.start(err)) << err;
    auto f = std::make_shared<hb::video::Frame>();
    f->width = 64, f->height = 36, f->stride = 256;
    f->data.assign(256 * 36, 1);
    std::vector<float> a(1602 * 2, 0.0f);
    for (int i = 0; i < 100; ++i) {
        r.pushVideo(f);
        r.pushAudio(a.data(), 1602);
        ASSERT_TRUE(waitFor([&] { return r.status().videoFrames >= static_cast<std::uint64_t>(i) + 1; }));
    }
    r.stop();
    const auto v = gFake->ofKind(Ev::Video);
    ASSERT_GE(v.size(), 99u);
    for (std::size_t i = 0; i < v.size(); ++i)
        EXPECT_EQ(v[i].pts, static_cast<std::int64_t>(i * 1001ull * 1000000000ull / 30000ull)) << i;
    // audio frames due per slot: round((k+1) * 48000 * 1001 / 30000) - never drifts from the video
    EXPECT_GE(r.status().audioFrames, 99u * 1601);
}

TEST_F(Fixture, LateVideoIsRepeatedToKeepConstantRate) {
    Recorder r(cfg(), &bus, &logger);
    std::string err;
    ASSERT_TRUE(r.start(err)) << err;
    auto mk = [](std::uint64_t idx) {
        auto f = std::make_shared<hb::video::Frame>();
        f->width = 64, f->height = 36, f->stride = 256, f->index = idx;
        f->data.assign(256 * 36, static_cast<std::uint8_t>(idx));
        return f;
    };
    for (std::uint64_t i = 0; i < 3; ++i) r.pushVideo(mk(i + 1));
    std::vector<float> a(16000 * 2, 0.0f);
    r.pushAudio(a.data(), 16000);  // 10 frame periods of audio, only 3 frames of video
    ASSERT_TRUE(waitFor([&] { return r.status().videoFrames >= 10; }));
    r.stop();
    const auto s = r.status();
    EXPECT_EQ(s.videoFrames, 10u);
    EXPECT_EQ(s.repeatedVideo, 7u);
    const auto v = gFake->ofKind(Ev::Video);
    ASSERT_EQ(v.size(), 10u);
    EXPECT_NE(v[0].ptr, v[1].ptr);
    for (std::size_t i = 3; i < 10; ++i) EXPECT_EQ(v[i].ptr, v[2].ptr) << "repeat of the last real frame " << i;
}

TEST_F(Fixture, VideoOnlyUsesInjectedClock) {
    std::atomic<std::int64_t> now{1000000000};
    auto c = cfg("fake-video");
    c.nowNs = [&] { return now.load(); };
    Recorder r(c, &bus, &logger);
    std::string err;
    ASSERT_TRUE(r.start(err)) << err;
    auto f = std::make_shared<hb::video::Frame>();
    f->width = 64, f->height = 36, f->stride = 256;
    f->data.assign(256 * 36, 9);
    r.pushVideo(f);  // t0 = 1 s
    std::this_thread::sleep_for(30ms);
    EXPECT_EQ(r.status().videoFrames, 0u);  // no time has passed on the injected clock
    now = 1000000000 + 100000000;           // +100 ms = 3 slots at 30 fps
    ASSERT_TRUE(waitFor([&] { return r.status().videoFrames >= 3; }));
    std::this_thread::sleep_for(30ms);
    EXPECT_EQ(r.status().videoFrames, 3u);
    EXPECT_EQ(r.status().repeatedVideo, 2u);
    r.stop();
    EXPECT_EQ(r.status().videoFrames, 3u);
}

TEST_F(Fixture, AudioStallIsFilledWithSilenceSoVideoDoesNotFreeze) {
    std::atomic<std::int64_t> now{5000000000ll};
    auto c = cfg();
    c.nowNs = [&] { return now.load(); };
    c.audioStallToleranceMs = 500;
    Recorder r(c, &bus, &logger);
    std::string err;
    ASSERT_TRUE(r.start(err)) << err;
    auto f = std::make_shared<hb::video::Frame>();
    f->width = 64, f->height = 36, f->stride = 256;
    f->data.assign(256 * 36, 9);
    r.pushVideo(f);
    std::this_thread::sleep_for(30ms);
    EXPECT_EQ(r.status().videoFrames, 0u);  // audio is the clock and it has not arrived yet
    now += 2000000000ll;                    // 2 s of wall time without a single audio sample
    ASSERT_TRUE(waitFor([&] { return r.status().videoFrames > 30; }));
    EXPECT_GT(r.status().silenceFrames, 0u);
    r.stop();
    EXPECT_EQ(r.status().state, Recorder::Status::State::Idle);
}

// ---------------------------------------------------------------- non-blocking producers
TEST_F(Fixture, PushNeverBlocksAndOverflowIsCounted) {
    Recorder r(cfg(), &bus, &logger);
    std::string err;
    ASSERT_TRUE(r.start(err)) << err;
    gFake->gate = true;  // the "disk" stalls inside writeVideo
    auto f = std::make_shared<hb::video::Frame>();
    f->width = 64, f->height = 36, f->stride = 256;
    f->data.assign(256 * 36, 3);
    std::vector<float> a(48000 * 2, 0.0f);
    r.pushVideo(f);
    r.pushAudio(a.data(), 1600);
    ASSERT_TRUE(waitFor([&] { return gFake->entered.load(); }));  // writer thread is now stuck
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 200; ++i) r.pushVideo(f);
    for (int i = 0; i < 5; ++i) r.pushAudio(a.data(), 48000);  // 5 s of audio into a 2 s ring
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    EXPECT_LT(ms, 500) << "push* blocked";
    const auto s = r.status();
    EXPECT_GE(s.droppedVideo, 190u);
    EXPECT_GE(s.droppedAudio, 3u * 48000);
    gFake->gate = false;
    r.stop();
    const auto fin = r.status();
    // dropped audio is replaced by silence, so the timeline keeps its length: every pushed sample is accounted
    EXPECT_EQ(fin.audioFrames, 1600u + 5u * 48000);
    EXPECT_GT(fin.silenceFrames, 0u);
    EXPECT_EQ(fin.state, Recorder::Status::State::Idle);
}

TEST_F(Fixture, WrongSizedFramesAreDroppedNotWritten) {
    Recorder r(cfg(), &bus, &logger);
    std::string err;
    ASSERT_TRUE(r.start(err)) << err;
    auto bad = std::make_shared<hb::video::Frame>();
    bad->width = 32, bad->height = 18, bad->stride = 128;
    bad->data.assign(128 * 18, 0);
    r.pushVideo(bad);
    auto shortData = std::make_shared<hb::video::Frame>();
    shortData->width = 64, shortData->height = 36, shortData->stride = 256;
    shortData->data.assign(10, 0);
    r.pushVideo(shortData);
    r.pushVideo(nullptr);
    EXPECT_EQ(r.status().droppedVideo, 2u);
    r.stop();
    EXPECT_EQ(r.status().videoFrames, 0u);
}

// ---------------------------------------------------------------- segmentation
TEST_F(Fixture, SplitByTimeLosesNoAudioAndRestartsTimestamps) {
    auto c = cfg();
    c.splitSeconds = 1;
    Recorder r(c, &bus, &logger);
    std::string err;
    ASSERT_TRUE(r.start(err)) << err;
    feedPaced(r, 105);  // 3.5 s
    r.stop();
    const auto s = r.status();
    EXPECT_EQ(s.state, Recorder::Status::State::Idle);
    EXPECT_EQ(s.segment, 4);
    EXPECT_EQ(s.videoFrames, 105u);
    EXPECT_EQ(s.audioFrames, 105u * 1600);
    EXPECT_EQ(gFake->audioFrames(), 105u * 1600);
    for (int f = 1; f <= 3; ++f) {
        EXPECT_EQ(gFake->ofKind(Ev::Video, f).size(), 30u) << f;
        EXPECT_EQ(gFake->audioFrames(f), 48000u) << f;
    }
    EXPECT_EQ(gFake->ofKind(Ev::Video, 4).size(), 15u);
    EXPECT_EQ(gFake->audioFrames(4), 24000u);
    for (int f = 1; f <= 4; ++f) {
        const auto v = gFake->ofKind(Ev::Video, f);
        EXPECT_EQ(v.front().pts, 0) << "file " << f << " starts at t = 0";
        const auto a = gFake->ofKind(Ev::Audio, f);
        EXPECT_EQ(a.front().pts, 0);
    }
    EXPECT_EQ(gFake->closes, 4);
    // file names
    const auto opens = gFake->ofKind(Ev::Open);
    ASSERT_EQ(opens.size(), 4u);
    EXPECT_NE(opens[1].path.find("_part002.fake"), std::string::npos) << opens[1].path;
    EXPECT_NE(opens[2].path.find("_part003.fake"), std::string::npos);
    EXPECT_EQ(opens[0].path.find("_part"), std::string::npos);
    EXPECT_EQ(count("recording.segment"), 3);
    EXPECT_EQ(count("recording.started"), 1);
    EXPECT_EQ(count("recording.stopped"), 1);
    for (const auto& o : opens) EXPECT_TRUE(std::filesystem::exists(hb::rec::detail::pathFromUtf8(o.path)));
}

TEST_F(Fixture, SplitBySizeUsesWriterByteCount) {
    gFake->bytesPerVideo = 1000;
    auto c = cfg();
    c.splitBytes = 10000;
    Recorder r(c, &bus, &logger);
    std::string err;
    ASSERT_TRUE(r.start(err)) << err;
    feedPaced(r, 35, 0, true);
    r.stop();
    EXPECT_EQ(r.status().segment, 4);
    EXPECT_EQ(gFake->ofKind(Ev::Video, 1).size(), 10u);
    EXPECT_EQ(gFake->ofKind(Ev::Video, 2).size(), 10u);
    EXPECT_EQ(gFake->ofKind(Ev::Video, 4).size(), 5u);
    EXPECT_EQ(gFake->audioFrames(), 35u * 1600);
    EXPECT_EQ(r.status().bytes, 35000u);
}

TEST_F(Fixture, AudioOnlySplitAtExactSampleBoundaries) {
    auto c = cfg("fake-audio");
    c.splitSeconds = 1;
    Recorder r(c, &bus, &logger);
    std::string err;
    ASSERT_TRUE(r.start(err)) << err;
    std::vector<float> a(1000 * 2, 0.0f);
    for (int i = 0; i < 120; ++i) {  // 2.5 s in 1000-frame pushes
        r.pushAudio(a.data(), 1000);
        std::this_thread::sleep_for(500us);
    }
    r.stop();
    EXPECT_EQ(r.status().audioFrames, 120000u);
    EXPECT_EQ(gFake->audioFrames(1), 48000u);
    EXPECT_EQ(gFake->audioFrames(2), 48000u);
    EXPECT_EQ(gFake->audioFrames(3), 24000u);
    EXPECT_EQ(r.status().segment, 3);
}

TEST_F(Fixture, WavAndAviAreCappedBelow4GB) {
    // The cap is applied through the extension: a fake profile with the "wav" extension must be capped.
    RecordingProfile p;
    p.id = "fake-wavlike";
    p.extension = "wav";
    p.hasVideo = false;
    p.kind = "audio";
    WriterRegistry::instance().add(p, [] { return std::unique_ptr<IMediaWriter>(new FakeWriter(gFake)); });
    gFake->bytesPerAudioFrame = 1u << 20;  // 1 MB per sample frame: absurdly large, forces the cap quickly
    auto c = cfg("fake-wavlike");
    c.splitBytes = 0;  // user asked for no split at all
    Recorder r(c, &bus, &logger);
    std::string err;
    ASSERT_TRUE(r.start(err)) << err;
    std::vector<float> a(2000 * 2, 0.0f);
    r.pushAudio(a.data(), 2000);
    ASSERT_TRUE(waitFor([&] { return r.status().audioFrames >= 2000; }));
    r.pushAudio(a.data(), 2000);
    ASSERT_TRUE(waitFor([&] { return r.status().audioFrames >= 4000; }));
    r.pushAudio(a.data(), 2000);  // the segment limit is checked before the next write
    ASSERT_TRUE(waitFor([&] { return r.status().audioFrames >= 6000; }));
    r.stop();
    EXPECT_GE(r.status().segment, 2);
}

// ---------------------------------------------------------------- failures
TEST_F(Fixture, OpenFailureIsReportedByStart) {
    gFake->failOpenOn = 1;
    Recorder r(cfg(), &bus, &logger);
    std::string err;
    EXPECT_FALSE(r.start(err));
    EXPECT_NE(err.find("falha injetada"), std::string::npos) << err;
    const auto s = r.status();
    EXPECT_EQ(s.state, Recorder::Status::State::Error);
    EXPECT_EQ(s.error, err);
    EXPECT_EQ(count("recording.error"), 1);
    EXPECT_EQ(count("recording.started"), 0);
    r.pushVideo(nullptr);  // harmless
    r.stop();
    EXPECT_EQ(r.status().state, Recorder::Status::State::Error);
}

TEST_F(Fixture, WriteFailureMidStreamGoesToErrorAndNeverAffectsCallers) {
    gFake->failVideoAfter = 5;
    Recorder r(cfg(), &bus, &logger);
    std::string err;
    ASSERT_TRUE(r.start(err)) << err;
    auto f = std::make_shared<hb::video::Frame>();
    f->width = 64, f->height = 36, f->stride = 256;
    f->data.assign(256 * 36, 1);
    std::vector<float> a(1600 * 2, 0.0f);
    for (int i = 0; i < 20; ++i) {
        r.pushVideo(f);
        r.pushAudio(a.data(), 1600);
        std::this_thread::sleep_for(2ms);
    }
    ASSERT_TRUE(waitFor([&] { return r.status().state == Recorder::Status::State::Error; }));
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 1000; ++i) {  // producers keep going: must stay instantaneous and safe
        r.pushVideo(f);
        r.pushAudio(a.data(), 1600);
    }
    EXPECT_LT(std::chrono::steady_clock::now() - t0, 500ms);
    const auto s = r.status();
    EXPECT_NE(s.error.find("falha injetada"), std::string::npos);
    EXPECT_EQ(gFake->closes, 1) << "the file must be closed after a failure";
    EXPECT_EQ(count("recording.error"), 1);
    r.stop();
    EXPECT_EQ(r.status().state, Recorder::Status::State::Error);
    EXPECT_EQ(count("recording.stopped"), 0);
}

TEST_F(Fixture, AudioWriteFailureIsAlsoHandled) {
    gFake->failAudioAfter = 3;
    Recorder r(cfg("fake-audio"), &bus, &logger);
    std::string err;
    ASSERT_TRUE(r.start(err)) << err;
    std::vector<float> a(1000 * 2, 0.0f);
    for (int i = 0; i < 30; ++i) {
        r.pushAudio(a.data(), 1000);
        std::this_thread::sleep_for(2ms);
    }
    ASSERT_TRUE(waitFor([&] { return r.status().state == Recorder::Status::State::Error; }));
    EXPECT_EQ(gFake->closes, 1);
    r.stop();
}

TEST_F(Fixture, FailureWhileRollingToNextSegment) {
    gFake->failOpenOn = 2;
    auto c = cfg();
    c.splitSeconds = 1;
    Recorder r(c, &bus, &logger);
    std::string err;
    ASSERT_TRUE(r.start(err)) << err;
    feedPaced(r, 40);
    ASSERT_TRUE(waitFor([&] { return r.status().state == Recorder::Status::State::Error; }));
    EXPECT_EQ(gFake->closes, 1);  // first segment finalised before the failure
    EXPECT_NE(r.status().error.find("falha injetada"), std::string::npos);
    r.stop();
}

TEST_F(Fixture, DiskFullAtStart) {
    auto c = cfg();
    c.minFreeBytes = ~0ull >> 1;  // more than any disk
    Recorder r(c, &bus, &logger);
    std::string err;
    EXPECT_FALSE(r.start(err));
    EXPECT_NE(err.find("Disco cheio"), std::string::npos) << err;
    EXPECT_EQ(r.status().state, Recorder::Status::State::Error);
    EXPECT_EQ(gFake->opens, 0);  // nothing was created
}

TEST_F(Fixture, DiskFillsDuringRecordingFinalisesFileAndEntersError) {
    std::atomic<std::int64_t> now{1000000000};
    std::atomic<std::uint64_t> freeBytes{10ull << 30};
    auto c = cfg();
    c.minFreeBytes = 500ull << 20;
    c.nowNs = [&] { return now.load(); };
    c.freeSpaceBytes = [&](const std::string&) { return freeBytes.load(); };
    Recorder r(c, &bus, &logger);
    std::string err;
    ASSERT_TRUE(r.start(err)) << err;
    EXPECT_EQ(r.status().freeBytes, 10ull << 30);
    feedPaced(r, 10);
    EXPECT_EQ(r.status().state, Recorder::Status::State::Recording);
    freeBytes = 100ull << 20;      // another program fills the disk
    now += 500000000;              // 0.5 s: the guard runs about once a second, so not yet
    std::this_thread::sleep_for(30ms);
    EXPECT_EQ(r.status().state, Recorder::Status::State::Recording);
    now += 600000000;              // > 1 s since the last check
    ASSERT_TRUE(waitFor([&] { return r.status().state == Recorder::Status::State::Error; }));
    const auto s = r.status();
    EXPECT_NE(s.error.find("Disco cheio"), std::string::npos) << s.error;
    EXPECT_EQ(s.freeBytes, 100ull << 20);
    EXPECT_EQ(gFake->closes, 1) << "file finalised safely";
    EXPECT_EQ(count("recording.error"), 1);
    const auto before = gFake->ofKind(Ev::Video).size();
    feedPaced(r, 3, 100, true);  // callers keep pushing; nothing more is written
    EXPECT_EQ(gFake->ofKind(Ev::Video).size(), before);
    r.stop();
}

TEST_F(Fixture, ConfigurationErrors) {
    std::string err;
    {
        auto c = cfg("does-not-exist");
        Recorder r(c);
        EXPECT_FALSE(r.start(err));
        EXPECT_NE(err.find("desconhecido"), std::string::npos) << err;
    }
    {
        auto c = cfg();
        c.video = VideoFormat{};
        Recorder r(c);
        EXPECT_FALSE(r.start(err));
        EXPECT_FALSE(err.empty());
    }
    {
        auto c = cfg();
        c.audio = AudioFormat{};
        Recorder r(c);
        EXPECT_FALSE(r.start(err));
    }
    {
        auto c = cfg();
        Recorder r(c);
        ASSERT_TRUE(r.start(err)) << err;
        EXPECT_FALSE(r.start(err));  // already recording
        r.stop();
        r.stop();  // idempotent
    }
}

// ---------------------------------------------------------------- concurrency
TEST_F(Fixture, TwoProducerThreadsAndStopDuringProduction) {
    Recorder r(cfg(), &bus, &logger);
    std::string err;
    ASSERT_TRUE(r.start(err)) << err;
    std::atomic<bool> run{true};
    auto f = std::make_shared<hb::video::Frame>();
    f->width = 64, f->height = 36, f->stride = 256;
    f->data.assign(256 * 36, 7);
    std::thread tv([&] {
        while (run) {
            r.pushVideo(f);
            std::this_thread::sleep_for(500us);
        }
    });
    std::thread ta([&] {
        std::vector<float> a(480 * 2, 0.01f);
        while (run) {
            r.pushAudio(a.data(), 480);
            std::this_thread::sleep_for(500us);
        }
    });
    std::thread poll([&] {
        while (run) {
            (void)r.status();
            std::this_thread::sleep_for(200us);
        }
    });
    std::this_thread::sleep_for(150ms);
    r.stop();  // while both producers are still running
    EXPECT_EQ(r.status().state, Recorder::Status::State::Idle);
    const auto s = r.status();
    EXPECT_GT(s.audioFrames, 0u);
    EXPECT_EQ(gFake->closes, 1);
    std::this_thread::sleep_for(20ms);  // producers keep pushing after stop: ignored
    run = false;
    tv.join();
    ta.join();
    poll.join();
    EXPECT_EQ(r.status().audioFrames, s.audioFrames);
    // and the recorder can be started again
    ASSERT_TRUE(r.start(err)) << err;
    feedPaced(r, 5);
    r.stop();
    EXPECT_EQ(r.status().videoFrames, 5u);
    EXPECT_EQ(gFake->opens, 2);
}

TEST_F(Fixture, DestructorStopsAndStopFromEventHandlerDoesNotDeadlock) {
    {
        Recorder r(cfg(), &bus, &logger);
        std::string err;
        ASSERT_TRUE(r.start(err)) << err;
        feedPaced(r, 3);
    }  // ~Recorder
    EXPECT_EQ(gFake->closes, 1);

    hb::core::EventBus bus2;
    Recorder* rp = nullptr;
    bus2.subscribe("recording.segment", [&](const hb::core::Event&) { rp->stop(); });
    auto c = cfg();
    c.splitSeconds = 1;
    Recorder r2(c, &bus2, &logger);
    rp = &r2;
    std::string err;
    ASSERT_TRUE(r2.start(err)) << err;
    feedPaced(r2, 40);
    r2.stop();
    EXPECT_EQ(r2.status().state, Recorder::Status::State::Idle);
}

// ---------------------------------------------------------------- file names
TEST_F(Fixture, NamesAreSanitisedAndNeverOverwrite) {
    auto c = cfg();
    c.nameTemplate = "../evil/{n}:x*";
    std::string err;
    std::string first;
    {
        Recorder r(c, &bus, &logger);
        ASSERT_TRUE(r.start(err)) << err;
        first = r.status().file;
        r.stop();
    }
    const auto p = hb::rec::detail::pathFromUtf8(first);
    EXPECT_EQ(p.parent_path(), std::filesystem::path(hb::rec::detail::pathFromUtf8(dir->folderUtf8())));
    EXPECT_EQ(first.find(".."), std::string::npos);
    EXPECT_NE(first.find("1_x_.fake"), std::string::npos) << first;
    // second recording with the same template: {n} moves on, nothing is overwritten
    c.nameTemplate = "fixed";
    std::string a, b;
    {
        Recorder r(c);
        ASSERT_TRUE(r.start(err)) << err;
        a = r.status().file;
        r.stop();
    }
    {
        Recorder r(c);
        ASSERT_TRUE(r.start(err)) << err;
        b = r.status().file;
        r.stop();
    }
    EXPECT_NE(a, b);
    EXPECT_NE(b.find("fixed (2).fake"), std::string::npos) << b;
    EXPECT_TRUE(std::filesystem::exists(hb::rec::detail::pathFromUtf8(a)));
    std::ifstream in(hb::rec::detail::pathFromUtf8(a));
    std::string content;
    in >> content;
    EXPECT_EQ(content, "fake");
}

TEST_F(Fixture, ProfileTunablesOverrideRegisteredOnes) {
    auto c = cfg();
    c.profile.id = "custom";
    c.profile.videoQuality = 33;
    c.profile.audioBitsPerSample = 16;
    c.profile.audioSampleRate = 48000;
    Recorder r(c);
    std::string err;
    ASSERT_TRUE(r.start(err)) << err;
    r.stop();
    EXPECT_EQ(gFake->lastP.videoQuality, 33);
    EXPECT_EQ(gFake->lastP.audioBitsPerSample, 16);
    EXPECT_EQ(gFake->lastP.id, "fake-av");  // identity comes from the registry
}

TEST_F(Fixture, UnicodeFolderAndTemplate) {
    auto c = cfg();
    c.folderUtf8 = dir->utf8("Gravações ção/日本語");
    c.nameTemplate = "Programa ação {n}";
    Recorder r(c);
    std::string err;
    ASSERT_TRUE(r.start(err)) << err;
    EXPECT_NE(r.status().file.find("Programa ação 1.fake"), std::string::npos);
    r.stop();
    EXPECT_TRUE(std::filesystem::exists(hb::rec::detail::pathFromUtf8(r.status().file)));
}

// ---------------------------------------------------------------- sample-rate conversion
TEST_F(Fixture, ResamplesProgramAudioToProfileRateBeforeTheWriter) {
    auto c = cfg("fake-audio");
    c.profile.id = "x";
    c.profile.audioSampleRate = 44100;
    Recorder r(c);
    std::string err;
    ASSERT_TRUE(r.start(err)) << err;
    EXPECT_EQ(gFake->lastA.sampleRate, 44100) << "writer must be opened with the delivered format";
    EXPECT_EQ(gFake->lastA.channels, 2);
    std::vector<float> a(4800 * 2, 0.0f);
    for (int i = 0; i < 20; ++i) {  // 2 s
        r.pushAudio(a.data(), 4800);
        std::this_thread::sleep_for(1ms);
    }
    r.stop();
    const auto written = r.status().audioFrames;
    EXPECT_NEAR(static_cast<double>(written), 2.0 * 44100, 200.0);
}

TEST_F(Fixture, RealWavRecording48kTo44k1KeepsDurationAndFrequency) {
    RecorderConfig c;
    c.profileId = "wav-16";
    c.folderUtf8 = dir->folderUtf8();
    c.audio = AudioFormat{48000, 2};
    c.profile.id = "wav-16";
    c.profile.audioBitsPerSample = 16;
    c.profile.audioSampleRate = 44100;
    c.minFreeBytes = 0;
    Recorder r(c, &bus, &logger);
    std::string err;
    ASSERT_TRUE(r.start(err)) << err;
    const std::string file = r.status().file;
    const std::size_t total = 48000 * 3;
    std::vector<float> sine(4800 * 2);
    for (std::size_t off = 0; off < total; off += 4800) {
        for (std::size_t i = 0; i < 4800; ++i) {
            const float v = static_cast<float>(0.5 * std::sin(2 * 3.14159265358979323846 * 1000.0 * static_cast<double>(off + i) / 48000.0));
            sine[i * 2] = v;
            sine[i * 2 + 1] = -v;
        }
        r.pushAudio(sine.data(), 4800);
        std::this_thread::sleep_for(1ms);
    }
    r.stop();
    ASSERT_EQ(r.status().state, Recorder::Status::State::Idle) << r.status().error;
    const auto wav = parseWav(readFile(hb::rec::detail::pathFromUtf8(file)));
    ASSERT_TRUE(wav.ok) << wav.error;
    EXPECT_EQ(wav.sampleRate, 44100);
    EXPECT_EQ(wav.channels, 2);
    const double dur = static_cast<double>(wav.frames) / 44100.0;
    std::printf("[ measure ] 48k->44.1k: %llu frames = %.4f s (expected 3.0)\n", static_cast<unsigned long long>(wav.frames), dur);
    EXPECT_NEAR(dur, 3.0, 0.01);
    // frequency from rising zero crossings of the left channel, skipping the filter start-up
    std::size_t first = 0, last = 0;
    int crossings = 0;
    const std::size_t lo = 2000, hi = wav.frames - 2000;
    for (std::size_t i = lo + 1; i < hi; ++i) {
        const int p = wav.ints[(i - 1) * 2], q = wav.ints[i * 2];
        if (p < 0 && q >= 0) {
            const double frac = static_cast<double>(-p) / static_cast<double>(q - p);
            const std::size_t pos = i;
            if (!crossings) first = pos;
            last = pos;
            (void)frac;
            ++crossings;
        }
    }
    ASSERT_GT(crossings, 100);
    const double hz = static_cast<double>(crossings - 1) * 44100.0 / static_cast<double>(last - first);
    std::printf("[ measure ] 1000 Hz tone after conversion: %.2f Hz, peak %.0f\n", hz, [&] {
        int pk = 0;
        for (std::size_t i = 5000; i < wav.ints.size() - 5000; ++i) pk = std::max(pk, std::abs(wav.ints[i]));
        return double(pk);
    }());
    EXPECT_NEAR(hz, 1000.0, 5.0);
    int pk = 0;
    for (std::size_t i = 5000; i < wav.ints.size() - 5000; ++i) pk = std::max(pk, std::abs(wav.ints[i]));
    EXPECT_NEAR(pk, 16384, 600);
}

// ---------------------------------------------------------------- real writers, 3 s of 1280x720@30 + 48 kHz
namespace {
struct RealRun {
    std::string file;
    Recorder::Status status;
};
}  // namespace

TEST_F(Fixture, RealAviMjpeg3SecondsReadBack) {
    RecorderConfig c;
    c.profileId = "avi-mjpeg";
    c.folderUtf8 = dir->folderUtf8();
    c.video = VideoFormat{1280, 720, 30, 1};
    c.audio = AudioFormat{48000, 2};
    c.minFreeBytes = 0;
    c.profile.id = "avi-mjpeg";
    c.profile.videoQuality = 85;
    c.profile.audioSampleRate = 48000;
    c.profile.audioBitsPerSample = 16;
    Recorder r(c, &bus, &logger);
    std::string err;
    ASSERT_TRUE(r.start(err)) << err;
    const std::string file = r.status().file;
    std::vector<std::shared_ptr<hb::video::Frame>> frames;
    std::vector<float> audio;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 90; ++i) {
        frames.push_back(makeFrame(1280, 720, static_cast<std::uint64_t>(i)));
        auto a = makeAudio(static_cast<std::size_t>(i) * 1600, 1600, 48000);
        audio.insert(audio.end(), a.begin(), a.end());
        r.pushVideo(frames.back());
        r.pushAudio(a.data(), 1600);
        ASSERT_TRUE(waitFor([&] { return r.status().videoFrames >= static_cast<std::uint64_t>(i) + 1; }, 20000));
    }
    r.stop();
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const auto s = r.status();
    ASSERT_EQ(s.state, Recorder::Status::State::Idle) << s.error;
    EXPECT_EQ(s.videoFrames, 90u);
    EXPECT_EQ(s.audioFrames, 144000u);
    EXPECT_EQ(s.droppedVideo, 0u);
    EXPECT_EQ(s.repeatedVideo, 0u);
    const auto bytes = readFile(hb::rec::detail::pathFromUtf8(file));
    EXPECT_EQ(s.bytes, bytes.size());
    const auto avi = parseAvi(bytes);
    ASSERT_TRUE(avi.ok) << avi.error;
    EXPECT_EQ(avi.videoFrames.size(), 90u);
    EXPECT_EQ(avi.audioFrames, 144000u);
    EXPECT_EQ(avi.width, 1280u);
    EXPECT_EQ(avi.height, 720u);
    double minPsnr = 99;
    for (int i : {0, 45, 89}) {
        const auto img = decodeJpeg(avi.videoFrames[static_cast<std::size_t>(i)]);
        ASSERT_TRUE(img.ok) << img.error;
        minPsnr = std::min(minPsnr, psnrRgbVsBgra(img.rgb, *frames[static_cast<std::size_t>(i)]));
    }
    EXPECT_GE(minPsnr, 30.0);
    // audio within 1 LSB of the 16-bit quantisation of the source
    std::size_t bad = 0;
    for (std::size_t i = 0; i < audio.size(); ++i) {
        const int ref = static_cast<int>(std::lrintf(std::max(-1.0f, std::min(1.0f, audio[i])) * 32768.0f));
        if (std::abs(avi.audio[i] - std::max(-32768, std::min(32767, ref))) > 1) ++bad;
    }
    EXPECT_EQ(bad, 0u);
    std::printf("[ measure ] AVI MJPEG q85 1280x720@30, 3 s: %zu bytes (%.2f MB, %.0f kB/s), PSNR(min of 3 frames) %.2f dB, encode+write wall %.2f s\n",
                bytes.size(), bytes.size() / 1048576.0, bytes.size() / 3.0 / 1024.0, minPsnr, wall);
}

TEST_F(Fixture, RealFlacAndWav3SecondsReadBack) {
    const std::size_t total = 48000 * 3;
    std::vector<float> audio = makeAudio(0, total, 48000, 2, 0.002f);
    std::string wavFile, flacFile;
    for (const char* id : {"wav", "flac"}) {
        RecorderConfig c;
        c.profileId = id;
        c.folderUtf8 = dir->folderUtf8();
        c.audio = AudioFormat{48000, 2};
        c.minFreeBytes = 0;
        Recorder r(c, &bus, &logger);
        std::string err;
        ASSERT_TRUE(r.start(err)) << err;
        (std::string(id) == "wav" ? wavFile : flacFile) = r.status().file;
        for (std::size_t off = 0; off < total; off += 960) {
            r.pushAudio(&audio[off * 2], 960);
            // pace like a real audio device would, so that the 2 s ring never overflows
            waitFor([&] { return r.status().audioFrames + 9600 >= off + 960; });
        }
        r.stop();
        ASSERT_EQ(r.status().state, Recorder::Status::State::Idle) << r.status().error;
        EXPECT_EQ(r.status().droppedAudio, 0u);
        EXPECT_EQ(r.status().audioFrames, total);
    }
    const auto wav = parseWav(readFile(hb::rec::detail::pathFromUtf8(wavFile)));
    const auto flac = parseFlac(readFile(hb::rec::detail::pathFromUtf8(flacFile)));
    ASSERT_TRUE(wav.ok) << wav.error;
    ASSERT_TRUE(flac.ok) << flac.error;
    EXPECT_EQ(wav.frames, total);
    EXPECT_EQ(flac.totalSamples, total);
    EXPECT_EQ(flac.bps, 24);
    // pushes were not paced: some audio may have been dropped and replaced by silence only if the ring
    // overflowed; here every push is small and the writer is fast, so both files must equal the source
    ASSERT_EQ(wav.ints.size(), audio.size());
    ASSERT_EQ(flac.samples.size(), audio.size());
    std::size_t diffWav = 0, diffFlac = 0;
    for (std::size_t i = 0; i < audio.size(); ++i) {
        const int ref = static_cast<int>(std::lrintf(audio[i] * 8388608.0f));
        if (std::abs(wav.ints[i] - ref) > 1) ++diffWav;
        if (flac.samples[i] != wav.ints[i]) ++diffFlac;
    }
    EXPECT_EQ(diffWav, 0u);
    EXPECT_EQ(diffFlac, 0u) << "FLAC must decode to exactly what the WAV contains";
    const auto wavSize = std::filesystem::file_size(hb::rec::detail::pathFromUtf8(wavFile));
    const auto flacSize = std::filesystem::file_size(hb::rec::detail::pathFromUtf8(flacFile));
    std::printf("[ measure ] 3 s stereo 24-bit 48 kHz: WAV %llu bytes, FLAC %llu bytes (ratio %.3f)\n",
                static_cast<unsigned long long>(wavSize), static_cast<unsigned long long>(flacSize), double(flacSize) / double(wavSize));
    EXPECT_LT(flacSize, wavSize);
}

TEST_F(Fixture, RealJpegSequence3SecondsReadBack) {
    RecorderConfig c;
    c.profileId = "jpeg-seq";
    c.folderUtf8 = dir->folderUtf8();
    c.video = VideoFormat{1280, 720, 30, 1};
    c.audio = AudioFormat{48000, 2};
    c.minFreeBytes = 0;
    Recorder r(c, &bus, &logger);
    std::string err;
    ASSERT_TRUE(r.start(err)) << err;
    const std::string folder = r.status().file;
    for (int i = 0; i < 30; ++i) {
        r.pushVideo(makeFrame(1280, 720, static_cast<std::uint64_t>(i)));
        auto a = makeAudio(static_cast<std::size_t>(i) * 1600, 1600, 48000);
        r.pushAudio(a.data(), 1600);
        ASSERT_TRUE(waitFor([&] { return r.status().videoFrames >= static_cast<std::uint64_t>(i) + 1; }, 20000));
    }
    r.stop();
    ASSERT_EQ(r.status().state, Recorder::Status::State::Idle) << r.status().error;
    const auto dp = hb::rec::detail::pathFromUtf8(folder);
    ASSERT_TRUE(std::filesystem::is_directory(dp));
    EXPECT_TRUE(std::filesystem::exists(dp / "frame_000030.jpg"));
    EXPECT_FALSE(std::filesystem::exists(dp / "frame_000031.jpg"));
    const auto wav = parseWav(readFile(dp / "audio.wav"));
    ASSERT_TRUE(wav.ok) << wav.error;
    EXPECT_EQ(wav.frames, 48000u);
    EXPECT_TRUE(decodeJpeg(readFile(dp / "frame_000001.jpg")).ok);
}

TEST_F(Fixture, RealAviSplitIntoSeveralPlayableFiles) {
    RecorderConfig c;
    c.profileId = "avi-mjpeg";
    c.folderUtf8 = dir->folderUtf8();
    c.video = VideoFormat{320, 180, 30, 1};
    c.audio = AudioFormat{48000, 2};
    c.minFreeBytes = 0;
    c.splitSeconds = 1;
    Recorder r(c, &bus, &logger);
    std::string err;
    ASSERT_TRUE(r.start(err)) << err;
    const std::string first = r.status().file;
    for (int i = 0; i < 75; ++i) {
        r.pushVideo(makeFrame(320, 180, static_cast<std::uint64_t>(i)));
        auto a = makeAudio(static_cast<std::size_t>(i) * 1600, 1600, 48000);
        r.pushAudio(a.data(), 1600);
        ASSERT_TRUE(waitFor([&] { return r.status().videoFrames >= static_cast<std::uint64_t>(i) + 1; }));
    }
    r.stop();
    ASSERT_EQ(r.status().state, Recorder::Status::State::Idle) << r.status().error;
    EXPECT_EQ(r.status().segment, 3);
    std::uint64_t v = 0, a = 0;
    int files = 0;
    for (const auto& e : std::filesystem::directory_iterator(dir->path)) {
        if (e.path().extension() != ".avi") continue;
        const auto avi = parseAvi(readFile(e.path()));
        ASSERT_TRUE(avi.ok) << e.path() << " " << avi.error;
        v += avi.videoFrames.size();
        a += avi.audioFrames;
        ++files;
    }
    EXPECT_EQ(files, 3);
    EXPECT_EQ(v, 75u);
    EXPECT_EQ(a, 75u * 1600);
}
