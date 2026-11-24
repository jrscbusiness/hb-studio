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
        // Never touch a real audio backend (PulseAudio/WASAPI/...): these tests exercise command
        // handling and the JSON contract, not real hardware, and a real backend can leak/hang in
        // a container with no audio server.
        o.opener = [](const audio_io::OpenRequest&, std::string& err) -> std::unique_ptr<audio_io::RawDevice> {
            err = "dispositivo simulado indisponível (teste).";
            return nullptr;
        };
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
        // Never touch a real audio backend (PulseAudio/WASAPI/...): these tests exercise command
        // handling and the JSON contract, not real hardware, and a real backend can leak/hang in
        // a container with no audio server.
        o.opener = [](const audio_io::OpenRequest&, std::string& err) -> std::unique_ptr<audio_io::RawDevice> {
            err = "dispositivo simulado indisponível (teste).";
            return nullptr;
        };
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

// ---- FEATURE: multitrack ----
TEST(MediaServiceIso, AudioChannelSessionWritesFolderLayoutAndManifest) {
    Rig r;
    EXPECT_EQ(r.svc->handle("rec|cfg|profile|wav-16"), "");
    EXPECT_EQ(r.svc->handle("rec|iso|ch|0|1"), "");
    EXPECT_EQ(r.svc->handle("rec|iso|cfg|afmt|wav-16"), "");
    EXPECT_EQ(r.svc->handle("rec|iso|cfg|tap|post"), "");
    EXPECT_NE(r.svc->handle("rec|iso|cfg|afmt|nao-existe"), "");
    EXPECT_NE(r.svc->handle("rec|iso|cfg|afmt|mp3"), "");
    EXPECT_EQ(r.svc->handle("rec|start"), "Gravação iniciada.");
    EXPECT_NE(r.svc->handle("rec|iso|ch|1|1"), "");                 // locked while recording
    std::string iso;
    for (std::uint64_t n = 0; n < 4; ++n) r.svc->tick(n, [&](const char* k, const std::string& j) { if (std::string(k) == "rec_iso") iso = j; });
    EXPECT_NE(iso.find("\"active\":true"), std::string::npos) << iso;
    EXPECT_NE(iso.find("\"id\":\"CH01\""), std::string::npos);
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    r.svc->handle("rec|stop");
    fs::path session;
    for (auto& e : fs::directory_iterator(r.dir / "rec")) if (e.is_directory()) session = e.path();
    ASSERT_FALSE(session.empty());
    EXPECT_TRUE(fs::exists(session / "program.wav"));
    EXPECT_TRUE(fs::exists(session / "session.json"));
    EXPECT_TRUE(fs::exists(session / "audio"));
    bool wav = false;
    for (auto& e : fs::directory_iterator(session / "audio")) wav = wav || e.path().extension() == ".wav";
    EXPECT_TRUE(wav);
    EXPECT_EQ(r.svc->isoFolder(), session.string());
}

TEST(MediaServiceIso, NothingSelectedKeepsTheFlatLegacyLayout) {
    Rig r;
    EXPECT_EQ(r.svc->handle("rec|cfg|profile|wav-16"), "");
    EXPECT_EQ(r.svc->handle("rec|cfg|template|flat"), "");
    EXPECT_EQ(r.svc->handle("rec|start"), "Gravação iniciada.");
    r.svc->handle("rec|stop");
    EXPECT_TRUE(fs::exists(r.dir / "rec" / "flat.wav"));
}

TEST(MediaServiceIso, CameraWithoutLiveSourceRefusesToStart) {
    Rig r;
    EXPECT_EQ(r.svc->handle("rec|iso|cam|2|1"), "");
    const std::string t = r.svc->handle("rec|start");
    EXPECT_NE(t.find("Não foi possível gravar"), std::string::npos) << t;
    EXPECT_FALSE(r.svc->recording());
}

TEST(MediaServiceIso, SelectionPersistsAndHostileInputIsHarmless) {
    {
        Rig r;
        r.svc->handle("rec|iso|ch|3|1");
        r.svc->handle("rec|iso|cam|5|1");
        r.svc->handle("rec|iso|cfg|rotmin|7");
        const fs::path keep = r.dir / "data" / "multitrack.json";
        ASSERT_TRUE(fs::exists(keep));
        for (const char* m : {"rec|iso", "rec|iso|cam", "rec|iso|cam|x|1", "rec|iso|cam|-1|1", "rec|iso|cam|999|1", "rec|iso|cfg|res|zzz",
                              "rec|iso|cfg|rotmin|abc", "rec|iso|cfg|vkbps|-5", "rec|iso|bogus|1|2"})
            EXPECT_NO_THROW(r.svc->handle(m));
        const std::string j = r.svc->iso().stateJson(false, (r.dir / "rec").string(), 8000, 500ull << 20);
        EXPECT_NE(j.find("\"rotmin\":7"), std::string::npos);
        EXPECT_NE(j.find("\"t\":\"rec_iso\""), std::string::npos);
    }
}
// ---- /FEATURE ----
