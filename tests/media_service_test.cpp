#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <map>
#include <thread>

#include "app/media_service.hpp"

using namespace hb;
namespace fs = std::filesystem;

namespace {
struct Rig {
    fs::path dir = fs::temp_directory_path() / ("hb_media_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    video::ProgramOutput program{{320, 180, {30, 1}}};
    std::unique_ptr<app::MediaService> svc;
    Rig() {
        fs::create_directories(dir);
        audio_io::EngineOptions o;
        o.lister = [](audio_io::Backend, std::string&) { return std::vector<audio_io::DeviceInfo>{}; };
        svc = std::make_unique<app::MediaService>(program, nullptr, nullptr, (dir / "data").string(), (dir / "rec").string(), o);
        svc->start();
        program.start();
    }
    ~Rig() {
        svc.reset();
        program.stop();
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
};
}  // namespace

TEST(MediaService, AudioCommandsChangeSnapshot) {
    Rig r;
    EXPECT_EQ(r.svc->handle("aud|strip|0|faderDb|-6"), "");
    EXPECT_EQ(r.svc->handle("aud|fxadd|0|hb.compressor"), "");
    EXPECT_EQ(r.svc->handle("aud|fxparam|0|0|threshold_db|-30"), "");
    const std::string j = r.svc->audio().snapshotJson(true);
    EXPECT_NE(j.find("hb.compressor"), std::string::npos);
    EXPECT_NE(j.find("\"faderDb\":-6"), std::string::npos);
    EXPECT_NE(r.svc->handle("aud|strip|0|faderDb|abc"), "");       // rejected, with a message
    EXPECT_NE(r.svc->handle("aud|fxadd|0|nao.existe"), "");
    EXPECT_NE(r.svc->handle("aud|fxdel|0|99"), "");
}

TEST(MediaService, TickPostsLevelsAndConfig) {
    Rig r;
    std::map<std::string, int> seen;
    for (std::uint64_t n = 0; n < 8; ++n) r.svc->tick(n, [&](const char* k, const std::string&) { ++seen[k]; });
    EXPECT_GE(seen["audiolv"], 8);
    EXPECT_EQ(seen["audio"], 1);
    EXPECT_EQ(seen["recprofiles"], 1);
    EXPECT_GE(seen["rec"], 2);
}

TEST(MediaService, RecordsWavAndFinalises) {
    Rig r;
    EXPECT_EQ(r.svc->handle("rec|cfg|profile|wav-16"), "");
    EXPECT_EQ(r.svc->handle("rec|cfg|template|teste"), "");
    EXPECT_EQ(r.svc->handle("rec|start"), "Gravação iniciada.");
    EXPECT_TRUE(r.svc->recording());
    EXPECT_NE(r.svc->handle("rec|cfg|profile|flac"), "");            // cannot change while recording
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    r.svc->handle("rec|stop");
    EXPECT_FALSE(r.svc->recording());
    fs::path f = r.dir / "rec" / "teste.wav";
    ASSERT_TRUE(fs::exists(f));
    EXPECT_GT(fs::file_size(f), 48000u * 4 / 2);   // at least ~0.5 s of 16-bit stereo
}

TEST(MediaService, RecordsVideoAviWithAudio) {
    Rig r;
    r.svc->handle("rec|cfg|profile|avi-mjpeg");
    r.svc->handle("rec|cfg|template|clip");
    ASSERT_EQ(r.svc->handle("rec|start"), "Gravação iniciada.");
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    r.svc->handle("rec|stop");
    fs::path f = r.dir / "rec" / "clip.avi";
    ASSERT_TRUE(fs::exists(f));
    EXPECT_GT(fs::file_size(f), 20000u);
}

TEST(MediaService, UnknownProfileRefusedAndSettingsPersist) {
    {
        Rig r;
        EXPECT_NE(r.svc->handle("rec|cfg|profile|xyz"), "");
        EXPECT_EQ(r.svc->handle("rec|cfg|audioKbps|128"), "");
        EXPECT_EQ(r.svc->handle("rec|cfg|splitMin|30"), "");
        EXPECT_EQ(r.svc->settings().audioBitrateKbps, 128);
        r.svc->stop();
        // Reload in a second service on the same data dir.
        video::ProgramOutput p2{{320, 180, {30, 1}}};
        audio_io::EngineOptions o;
        o.lister = [](audio_io::Backend, std::string&) { return std::vector<audio_io::DeviceInfo>{}; };
        app::MediaService s2(p2, nullptr, nullptr, (r.dir / "data").string(), (r.dir / "rec").string(), o);
        s2.start();
        EXPECT_EQ(s2.settings().audioBitrateKbps, 128);
        EXPECT_EQ(s2.settings().splitMinutes, 30);
        s2.stop();
    }
}

TEST(MediaService, HostileCommandsNeverCrash) {
    Rig r;
    for (const char* m : {"aud", "aud|", "aud|strip", "aud|strip|-1|faderDb|0", "aud|strip|99|x|1", "aud|fxparam|0|0|", "rec|cfg|audioKbps|-5",
                          "rec|cfg|folder|", "aud|master|faderDb|nan", "aud|input|0|device|zzz", "rec|bogus", "x|y|z"})
        r.svc->handle(m);
    SUCCEED();
}
