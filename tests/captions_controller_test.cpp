#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <thread>

#include "captions/controller.hpp"
#include "core/json.hpp"
#include "core/sha256.hpp"

using namespace hb::captions;
namespace fs = std::filesystem;

namespace {
struct TmpDir {
    fs::path p;
    TmpDir() {
        p = fs::temp_directory_path() / ("hb_captions_ctl_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        fs::create_directories(p);
    }
    ~TmpDir() { std::error_code ec; fs::remove_all(p, ec); }
};

struct FakeDl : IDownloader {
    std::string payload = "modelo-falso-de-teste";
    bool download(const std::string&, const std::string& dest, const std::function<bool(std::uint64_t, std::uint64_t)>& progress,
                  std::string&) override {
        progress(payload.size(), payload.size());
        std::ofstream f(fs::path(reinterpret_cast<const char8_t*>(dest.c_str())), std::ios::binary);
        f << payload;
        return true;
    }
};

// Fake program-audio source: remembers the consumer so the test can push audio like the engine thread does.
struct FakeAudio {
    std::function<void(const float*, std::size_t)> consumer;
    std::uint64_t add(std::function<void(const float*, std::size_t)> c) { consumer = std::move(c); return 7; }
    void remove(std::uint64_t id) { if (id == 7) consumer = nullptr; }
};

hb::core::json::Value parseJson(const std::string& s) {
    hb::core::json::Value v;
    EXPECT_TRUE(hb::core::json::parse(s, v)) << s;
    return v;
}
bool waitFor(const std::function<bool()>& pred, int ms = 5000) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (!pred()) {
        if (std::chrono::steady_clock::now() > end) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return true;
}

struct Rig {
    TmpDir dir;
    hb::core::EventBus bus;
    FakeDl dl;
    FakeAudio audio;
    std::unique_ptr<CaptionController> ctl;
    explicit Rig(bool whisper = true) {
        ControllerDeps d;
        d.bus = &bus;
        d.settingsPath = (dir.p / "captions.json").string();
        d.modelsDir = (dir.p / "models").string();
        d.srtFolder = [this] { return (dir.p / "rec").string(); };
        d.downloader = &dl;
        d.addProgramConsumer = [this](auto c) { return audio.add(std::move(c)); };
        d.removeProgramConsumer = [this](std::uint64_t id) { audio.remove(id); };
        d.timestamp = [] { return std::string("20260101_120000"); };
        ModelInfo m;
        m.id = "leve";
        m.label = "Leve";
        m.filename = "fake.bin";
        m.url = "https://example.invalid/fake.bin";
        m.sha256 = hb::core::Sha256::hex(dl.payload);
        m.sizeBytes = dl.payload.size();
        d.catalog = {m};
        if (whisper)
            d.makeWhisper = [](const std::string&) {
                return std::make_unique<FakeRecognizer>([](const RecognizeRequest&, int) { return std::string("olá legenda"); }, false);
            };
        ctl = std::make_unique<CaptionController>(std::move(d));
    }
    void pushSpeech() {
        std::vector<float> blk(480 * 2);
        long n = 0;
        auto feed = [&](int ms, float amp) {
            for (int b = 0; b < ms / 10; ++b) {
                for (int i = 0; i < 480; ++i, ++n) blk[i * 2] = blk[i * 2 + 1] = amp * std::sin(2.0f * 3.14159265f * 220.0f * static_cast<float>(n) / 48000.0f);
                if (audio.consumer) audio.consumer(blk.data(), 480);
            }
        };
        feed(1000, 0.2f);
        feed(1500, 0.0f);
    }
};
}  // namespace

TEST(CaptionsController, CommandsAreOnlyHandledWhenPrefixed) {
    Rig r;
    EXPECT_FALSE(r.ctl->handleCommand("rec|start"));
    EXPECT_TRUE(r.ctl->handleCommand("cap|bogus"));
}

TEST(CaptionsController, InitialStateJsonIsValidAndComplete) {
    Rig r;
    const auto v = parseJson(r.ctl->stateJson());
    EXPECT_EQ(v.get("t")->s, "cap");
    EXPECT_FALSE(v.get("on")->b);
    EXPECT_EQ(v.get("state")->s, "stopped");
    EXPECT_EQ(v.get("cfg")->get("language")->s, "pt");
    ASSERT_EQ(v.get("models")->a.size(), 1u);
    EXPECT_FALSE(v.get("models")->a[0].get("installed")->b);
    EXPECT_FALSE(r.ctl->pollJson(false).empty());   // first poll: changed
    EXPECT_TRUE(r.ctl->pollJson(false).empty());    // unchanged -> ""
    EXPECT_FALSE(r.ctl->pollJson(true).empty());
}

TEST(CaptionsController, CannotStartWithoutModelThenDownloadsAndRuns) {
    Rig r;
    r.ctl->handleCommand("cap|on");
    EXPECT_FALSE(r.ctl->running());
    EXPECT_NE(parseJson(r.ctl->stateJson()).get("msg")->s.find("Baixe"), std::string::npos);

    r.ctl->handleCommand("cap|dl|leve");
    ASSERT_TRUE(waitFor([&] {
        const auto v = parseJson(r.ctl->stateJson());
        return !v.get("dl")->get("active")->b && v.get("models")->a[0].get("installed")->b;
    }));

    r.ctl->handleCommand("cap|on");
    ASSERT_TRUE(r.ctl->running());
    ASSERT_TRUE(waitFor([&] { return parseJson(r.ctl->stateJson()).get("state")->s == "listening"; }));
    r.pushSpeech();
    ASSERT_TRUE(waitFor([&] { return !parseJson(r.ctl->stateJson()).get("lines")->a.empty(); }));
    const auto v = parseJson(r.ctl->stateJson());
    EXPECT_EQ(v.get("lines")->a[0].s, "Olá legenda.");
    EXPECT_EQ(v.get("partial")->s, "");

    r.ctl->handleCommand("cap|off");
    EXPECT_FALSE(r.ctl->running());
    EXPECT_FALSE(r.audio.consumer);   // consumer unregistered
    const auto after = parseJson(r.ctl->stateJson());
    EXPECT_EQ(after.get("state")->s, "stopped");
    // no recording was closed: the .srt goes to the fallback folder with the injected timestamp
    const std::string srt = after.get("srtFile")->s;
    ASSERT_FALSE(srt.empty());
    EXPECT_NE(srt.find("HBStudio_legendas_20260101_120000.srt"), std::string::npos);
    std::string content;
    {
        std::ifstream f(fs::path(srt), std::ios::binary);
        content.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    EXPECT_NE(content.find("Olá legenda."), std::string::npos);
    EXPECT_NE(content.find("-->"), std::string::npos);
}

TEST(CaptionsController, SrtGoesNextToClosedProgramRecordingAndSavingCanBeDisabled) {
    Rig r;
    r.ctl->handleCommand("cap|dl|leve");
    ASSERT_TRUE(waitFor([&] { return parseJson(r.ctl->stateJson()).get("models")->a[0].get("installed")->b; }));
    r.ctl->handleCommand("cap|on");
    ASSERT_TRUE(waitFor([&] { return parseJson(r.ctl->stateJson()).get("state")->s == "listening"; }));
    r.pushSpeech();
    ASSERT_TRUE(waitFor([&] { return !parseJson(r.ctl->stateJson()).get("lines")->a.empty(); }));
    const fs::path rec = r.dir.p / "meu video.mp4";
    r.bus.publish({"recording.fileClosed", {{"path", rec.string()}, {"kind", "program"}}});
    r.bus.publish({"recording.fileClosed", {{"path", (r.dir.p / "iso.mp4").string()}, {"kind", "iso"}}});   // ISO tracks ignored
    r.ctl->handleCommand("cap|off");
    EXPECT_TRUE(fs::exists(r.dir.p / "meu video.srt"));
    EXPECT_FALSE(fs::exists(r.dir.p / "iso.srt"));

    // saveSrt off: nothing written
    fs::remove(r.dir.p / "meu video.srt");
    r.ctl->handleCommand("cap|set|saveSrt|0");
    r.ctl->handleCommand("cap|on");
    ASSERT_TRUE(waitFor([&] { return parseJson(r.ctl->stateJson()).get("state")->s == "listening"; }));
    r.pushSpeech();
    ASSERT_TRUE(waitFor([&] { return !parseJson(r.ctl->stateJson()).get("lines")->a.empty(); }));
    r.bus.publish({"recording.fileClosed", {{"path", rec.string()}, {"kind", "program"}}});
    r.ctl->handleCommand("cap|off");
    EXPECT_FALSE(fs::exists(r.dir.p / "meu video.srt"));
}

TEST(CaptionsController, SettingsAreValidatedPersistedAndReloaded) {
    Rig r;
    r.ctl->handleCommand("cap|set|language|en");
    r.ctl->handleCommand("cap|set|language|klingon");
    EXPECT_EQ(parseJson(r.ctl->stateJson()).get("cfg")->get("language")->s, "en");
    EXPECT_NE(parseJson(r.ctl->stateJson()).get("msg")->s.find("inválido"), std::string::npos);
    r.ctl->handleCommand("cap|set|cloudKey|abc|def");   // value may contain '|'
    const auto v = parseJson(r.ctl->stateJson());
    EXPECT_TRUE(v.get("cfg")->get("hasKey")->b);
    EXPECT_EQ(r.ctl->stateJson().find("abc"), std::string::npos);   // the key is never sent to the UI
    CaptionSettings s;
    ASSERT_TRUE(loadSettings((r.dir.p / "captions.json").string(), s));
    EXPECT_EQ(s.language, "en");
    EXPECT_EQ(s.cloudKey, "abc|def");
}

TEST(CaptionsController, WithoutWhisperBuildTheLocalEngineExplainsAndCloudNeedsValidConfig) {
    Rig r(false);
    r.ctl->handleCommand("cap|on");
    EXPECT_FALSE(r.ctl->running());
    EXPECT_NE(parseJson(r.ctl->stateJson()).get("msg")->s.find("whisper"), std::string::npos);
    r.ctl->handleCommand("cap|set|engine|cloud");
    r.ctl->handleCommand("cap|on");   // no http client in this rig
    EXPECT_FALSE(r.ctl->running());
}

TEST(CaptionsController, CaptionsFromOtherPublishersDoNotPolluteTranscriptWhenOff) {
    Rig r;
    r.bus.publish({"caption.text", {{"text", "externo"}, {"final", "1"}, {"lang", "pt"}}});
    EXPECT_TRUE(parseJson(r.ctl->stateJson()).get("lines")->a.empty());
}
