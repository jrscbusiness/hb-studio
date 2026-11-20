#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <random>

#include "audio_io_test_util.hpp"
#include "core/json.hpp"

using namespace hb::audio_io;
using namespace hb::audio_io::test;
namespace fs = std::filesystem;

namespace {
struct TempDir {
    fs::path p;
    TempDir() {
        static int n = 0;
        p = fs::temp_directory_path() / ("hb_audio_io_test_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
                                         "_" + std::to_string(++n));
        fs::create_directories(p);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(p, ec);
    }
    std::string file(const std::string& name) const { return (p / name).string(); }
};

void write(const std::string& path, const std::string& text) {
    std::ofstream f(path, std::ios::binary);
    f << text;
}

void expectSane(AudioEngine& e) {
    const auto s = e.snapshot();
    ASSERT_EQ(s.strips.size(), 8u);
    for (const auto& st : s.strips) {
        EXPECT_GE(st.trimDb, -60.0f);
        EXPECT_LE(st.trimDb, 24.0f);
        EXPECT_LE(st.faderDb, 10.0f);
        EXPECT_GE(st.pan, -1.0f);
        EXPECT_LE(st.pan, 1.0f);
        EXPECT_GE(st.delayMs, 0.0f);
        EXPECT_LE(st.delayMs, 2000.0f);
        EXPECT_LE(st.effects.size(), 8u);
        EXPECT_LE(st.name.size(), 32u);
        EXPECT_FALSE(st.name.empty());
    }
    EXPECT_GE(s.master.ceilingDb, -12.0f);
    EXPECT_LE(s.master.ceilingDb, 0.0f);
    EXPECT_GE(s.monitor.level, 0.0f);
    EXPECT_LE(s.monitor.level, 1.0f);
    hb::core::json::Value v;
    EXPECT_TRUE(hb::core::json::parse(e.snapshotJson(true), v));
    EXPECT_TRUE(hb::core::json::parse(e.settingsJson(), v));
    for (int i = 0; i < 3; ++i) e.processBlock();  // still runs
}

void configure(Rig& r) {
    auto& e = *r.engine;
    e.setStripParam(0, "name", "Narrador");
    e.setStripInput(0, "device", "cap:wasapi:{0.0.1.00000000}.{abc}");
    e.setStripParam(0, "trimDb", 6.5);
    e.setStripParam(0, "faderDb", -12.25);
    e.setStripParam(0, "pan", -0.4);
    e.setStripParam(0, "phase", 1);
    e.setStripParam(0, "delayMs", 123.5);
    e.setStripQuality(0, "high");
    e.setStripParam(0, "toMonitor", 1);
    e.addEffect(0, "test.gain");
    e.addEffect(0, "test.invert");
    e.setEffectParam(0, 0, "gain", 0.37);
    e.setEffectParam(0, 0, "mode", 2);
    e.setEffectBypass(0, 1, true);
    e.setStripInput(1, "device", "loop:wasapi:{0.0.0.00000000}.{def}");
    e.setStripParam(1, "mute", 1);
    e.setStripParam(1, "solo", 1);
    e.setStripParam(1, "mono", 1);
    e.setStripParam(1, "faderDb", -200);  // -inf
    e.setStripQuality(1, "linear");
    e.setStripInput(2, "external", "pc-audio");
    e.setStripParam(2, "toProgram", 0);
    e.setMasterParam("faderDb", -3.5);
    e.setMasterParam("limiter", 0);
    e.setMasterParam("ceilingDb", -2.5);
    e.setMonitor("level", 0.4);
    e.setMonitor("mute", 1);
    e.setMonitor("deviceId", "play:wasapi:{0.0.0.00000000}.{headset}");
}
}  // namespace

TEST(AudioIoSettings, RoundTripThroughFile) {
    TempDir d;
    Rig a;
    configure(a);
    const std::string path = d.file("audio.json");
    ASSERT_TRUE(a.engine->saveSettings(path));
    EXPECT_FALSE(fs::exists(path + ".tmp"));  // atomic: temp file renamed away
    Rig b;
    ASSERT_TRUE(b.engine->loadSettings(path)) << b.engine->lastError();
    EXPECT_EQ(a.engine->settingsJson(), b.engine->settingsJson());
    const auto s = b.engine->snapshot();
    EXPECT_EQ(s.strips[0].name, "Narrador");
    EXPECT_EQ(s.strips[0].inputKind, InputKind::Device);
    EXPECT_EQ(s.strips[0].inputId, "cap:wasapi:{0.0.1.00000000}.{abc}");
    EXPECT_FLOAT_EQ(s.strips[0].trimDb, 6.5f);
    EXPECT_FLOAT_EQ(s.strips[0].faderDb, -12.25f);
    EXPECT_FLOAT_EQ(s.strips[0].pan, -0.4f);
    EXPECT_TRUE(s.strips[0].phase);
    EXPECT_FLOAT_EQ(s.strips[0].delayMs, 123.5f);
    EXPECT_EQ(s.strips[0].quality, Quality::High);
    ASSERT_EQ(s.strips[0].effects.size(), 2u);
    EXPECT_FLOAT_EQ(s.strips[0].effects[0].values[0], 0.37f);
    EXPECT_FLOAT_EQ(s.strips[0].effects[0].values[1], 2.0f);
    EXPECT_TRUE(s.strips[0].effects[1].bypass);
    EXPECT_EQ(s.strips[1].inputId, "loop:wasapi:{0.0.0.00000000}.{def}");
    EXPECT_TRUE(std::isinf(s.strips[1].faderDb) && s.strips[1].faderDb < 0);
    EXPECT_EQ(s.strips[2].inputKind, InputKind::External);
    EXPECT_FALSE(s.strips[2].toProgram);
    EXPECT_FLOAT_EQ(s.master.faderDb, -3.5f);
    EXPECT_FALSE(s.master.limiter);
    EXPECT_FLOAT_EQ(s.master.ceilingDb, -2.5f);
    EXPECT_EQ(s.monitor.deviceId, "play:wasapi:{0.0.0.00000000}.{headset}");
    EXPECT_FLOAT_EQ(s.monitor.level, 0.4f);
    EXPECT_TRUE(s.monitor.mute);
    EXPECT_TRUE(b.engine->settingsWarnings().empty());
    // the file is versioned JSON
    hb::core::json::Value v;
    std::ifstream f(path);
    std::string txt((std::istreambuf_iterator<char>(f)), {});
    ASSERT_TRUE(hb::core::json::parse(txt, v));
    EXPECT_EQ(v.get("schema")->n, 1);
}

TEST(AudioIoSettings, SaveReplacesExistingFileAndFailsGracefully) {
    TempDir d;
    Rig a;
    const std::string path = d.file("a.json");
    write(path, "old content");
    ASSERT_TRUE(a.engine->saveSettings(path));
    hb::core::json::Value v;
    std::ifstream f(path);
    std::string txt((std::istreambuf_iterator<char>(f)), {});
    EXPECT_TRUE(hb::core::json::parse(txt, v));
    EXPECT_FALSE(a.engine->saveSettings((d.p / "no" / "such" / "dir" / "x.json").string()));
    EXPECT_FALSE(a.engine->saveSettings(""));
    EXPECT_FALSE(a.engine->lastError().empty());
}

TEST(AudioIoSettings, MissingOrCorruptFilesKeepTheCurrentState) {
    TempDir d;
    Rig r;
    r.engine->setStripParam(0, "trimDb", 3);
    EXPECT_FALSE(r.engine->loadSettings(d.file("nao-existe.json")));
    write(d.file("empty.json"), "");
    EXPECT_FALSE(r.engine->loadSettings(d.file("empty.json")));
    write(d.file("trunc.json"), "{\"schema\":1,\"strips\":[{\"name\":\"abc");
    EXPECT_FALSE(r.engine->loadSettings(d.file("trunc.json")));
    EXPECT_FALSE(r.engine->lastError().empty());
    EXPECT_FLOAT_EQ(r.engine->snapshot().strips[0].trimDb, 3.0f);
    expectSane(*r.engine);
    // fresh engine: stays at the defaults
    Rig fresh;
    EXPECT_FALSE(fresh.engine->loadSettings(d.file("trunc.json")));
    EXPECT_EQ(fresh.engine->snapshot().strips[0].name, "MIC 1");
}

TEST(AudioIoSettings, HostileFilesNeverCrashAndAreSanitised) {
    Rig r;
    std::vector<std::string> bad;
    bad.push_back("[]");
    bad.push_back("null");
    bad.push_back("42");
    bad.push_back("\"text\"");
    bad.push_back(std::string(5000, '['));                                    // absurd nesting
    bad.push_back(std::string(5 * 1024 * 1024, 'x'));                         // over the size limit
    {
        std::string bin;
        std::mt19937 g(7);
        for (int i = 0; i < 4096; ++i) bin.push_back(static_cast<char>(g()));
        bad.push_back(bin);
    }
    // Well-formed JSON of the wrong shape is not an error: it simply means "defaults".
    for (const char* t : {"{\"schema\":1,\"strips\":5}", "{\"schema\":\"x\",\"strips\":{\"a\":1}}", "{}"}) {
        EXPECT_TRUE(r.engine->applySettingsJson(t)) << t;
        expectSane(*r.engine);
        EXPECT_EQ(r.engine->snapshot().strips[0].name, "MIC 1");
    }
    for (const auto& t : bad) {
        EXPECT_FALSE(r.engine->applySettingsJson(t)) << t.substr(0, 40);
        expectSane(*r.engine);
    }

    // Valid JSON, hostile values: loads, but everything is clamped / ignored.
    std::string strips = "[";
    for (int i = 0; i < 300; ++i) {
        if (i) strips += ',';
        strips += "{\"name\":\"" + std::string(i == 0 ? 100000 : 3, 'n') + "\",\"trimDb\":1e30,\"faderDb\":1e300,\"pan\":-9e9,"
                  "\"delayMs\":1e999,\"mute\":\"yes\",\"quality\":\"warp\",\"input\":{\"kind\":\"device\",\"id\":\"" +
                  std::string(i == 1 ? 100000 : 3, 'i') +
                  "\"},\"effects\":[{\"type\":\"../../etc\"},{\"type\":5},7,null,{\"type\":\"test.gain\",\"params\":{\"gain\":1e30,"
                  "\"nope\":3,\"mode\":-5,\"bad id!\":1}}";
        for (int k = 0; k < 20; ++k) strips += ",{\"type\":\"test.invert\"}";
        strips += "]}";
    }
    strips += "]";
    const std::string hostile = "{\"schema\":1,\"strips\":" + strips +
                                ",\"master\":{\"faderDb\":-1e300,\"limiter\":3,\"ceilingDb\":5000},"
                                "\"monitor\":{\"level\":99,\"deviceId\":\"cap:not-playback\"}}";
    ASSERT_TRUE(r.engine->applySettingsJson(hostile)) << r.engine->lastError();
    expectSane(*r.engine);
    const auto s = r.engine->snapshot();
    EXPECT_EQ(s.strips[0].trimDb, 24.0f);
    EXPECT_EQ(s.strips[0].pan, -1.0f);
    EXPECT_EQ(s.strips[0].delayMs, 0.0f);
    EXPECT_EQ(s.strips[0].quality, Quality::Medium);
    EXPECT_EQ(s.strips[1].inputKind, InputKind::None);  // id too long: rejected
    EXPECT_FALSE(s.strips[0].mute);                       // "yes" is not a bool
    EXPECT_EQ(s.strips[0].effects.size(), 8u);
    EXPECT_EQ(s.strips[0].effects[0].type, "test.gain");  // "../../etc" and non-strings dropped
    EXPECT_EQ(s.strips[0].effects[0].values[0], 4.0f);
    EXPECT_EQ(s.monitor.deviceId, "");
    EXPECT_FALSE(r.engine->settingsWarnings().empty());
}

TEST(AudioIoSettings, FutureSchemaIsReadLeniently) {
    Rig r;
    const std::string text =
        "{\"schema\":99,\"newThing\":{\"a\":1},\"strips\":[{\"name\":\"Futuro\",\"trimDb\":2,\"holo\":true}],"
        "\"master\":{\"faderDb\":-1}}";
    ASSERT_TRUE(r.engine->applySettingsJson(text));
    const auto w = r.engine->settingsWarnings();
    ASSERT_FALSE(w.empty());
    EXPECT_NE(w[0].find("mais nova"), std::string::npos) << w[0];
    EXPECT_EQ(r.engine->snapshot().strips[0].name, "Futuro");
    EXPECT_FLOAT_EQ(r.engine->snapshot().strips[0].trimDb, 2.0f);
    EXPECT_EQ(r.engine->snapshot().strips[1].name, "MIC 2");  // absent strips fall back to defaults
    expectSane(*r.engine);
}

TEST(AudioIoSettings, MigratesTheUnversionedPrototypeFormat) {
    Rig r;
    const std::string text =
        "{\"strips\":[{\"name\":\"Antigo\",\"trim\":3,\"fader\":-6,\"phase\":true,\"delay\":40,\"device\":\"cap:x:1\","
        "\"effects\":[{\"typeId\":\"test.gain\",\"params\":{\"gain\":0.5}}]}]}";
    ASSERT_TRUE(r.engine->applySettingsJson(text));
    const auto s = r.engine->snapshot();
    EXPECT_EQ(s.strips[0].name, "Antigo");
    EXPECT_FLOAT_EQ(s.strips[0].trimDb, 3.0f);
    EXPECT_FLOAT_EQ(s.strips[0].faderDb, -6.0f);
    EXPECT_TRUE(s.strips[0].phase);
    EXPECT_FLOAT_EQ(s.strips[0].delayMs, 40.0f);
    EXPECT_EQ(s.strips[0].inputId, "cap:x:1");
    ASSERT_EQ(s.strips[0].effects.size(), 1u);
    EXPECT_FLOAT_EQ(s.strips[0].effects[0].values[0], 0.5f);
    const auto w = r.engine->settingsWarnings();
    ASSERT_FALSE(w.empty());
    EXPECT_NE(w[0].find("migrada"), std::string::npos);
    // saving writes the current schema
    hb::core::json::Value v;
    ASSERT_TRUE(hb::core::json::parse(r.engine->settingsJson(), v));
    EXPECT_EQ(v.get("schema")->n, 1);
}

TEST(AudioIoSettings, UnknownEffectIsKeptAsMissingWithoutBreakingTheRest) {
    Rig r;
    const std::string text =
        "{\"schema\":1,\"strips\":[{\"name\":\"A\",\"effects\":[{\"type\":\"test.gain\",\"params\":{\"gain\":0.5}},"
        "{\"type\":\"plugin.reverb-9\",\"bypass\":true,\"params\":{\"mix\":0.3,\"size\":0.9}},"
        "{\"type\":\"test.invert\"}]}]}";
    ASSERT_TRUE(r.engine->applySettingsJson(text));
    auto s = r.engine->snapshot();
    ASSERT_EQ(s.strips[0].effects.size(), 3u);
    EXPECT_FALSE(s.strips[0].effects[0].missing);
    EXPECT_TRUE(s.strips[0].effects[1].missing);
    EXPECT_EQ(s.strips[0].effects[1].type, "plugin.reverb-9");
    EXPECT_TRUE(s.strips[0].effects[1].bypass);
    EXPECT_FALSE(s.strips[0].effects[2].missing);
    EXPECT_FALSE(r.engine->setEffectParam(0, 1, "mix", 0.5));  // cannot edit a missing effect
    EXPECT_NE(r.engine->snapshotJson(true).find("\"missing\":true"), std::string::npos);
    r.engine->setStripInput(0, "external", "x");
    r.engine->registerExternalSource("x", std::make_shared<ConstSource>(0.4f, 0.4f));
    r.engine->setStripParam(0, "pan", -1);
    r.run(6);
    EXPECT_NEAR(r.lastL(), -0.2f, 2e-5f);  // gain 0.5 and invert applied, the missing one passes through

    // the missing entry survives a save/load cycle with its parameters
    const std::string saved = r.engine->settingsJson();
    EXPECT_NE(saved.find("plugin.reverb-9"), std::string::npos);
    EXPECT_NE(saved.find("\"mix\":0.3"), std::string::npos);
    Rig r2;
    ASSERT_TRUE(r2.engine->applySettingsJson(saved));
    EXPECT_EQ(r2.engine->settingsJson(), saved);
    EXPECT_FALSE(r2.engine->settingsWarnings().empty());
}

TEST(AudioIoSettings, LoadingAppliesToARunningEngineAndRebuildsChains) {
    Rig a;
    a.engine->addEffect(0, "test.gain");
    a.engine->addEffect(0, "test.gain");
    a.engine->addEffect(1, "test.invert");
    const std::string one = a.engine->settingsJson();
    Rig b;
    b.engine->addEffect(0, "test.invert");
    b.engine->addEffect(3, "test.invert");
    b.engine->setStripParam(2, "mute", 1);
    b.run(2);
    ASSERT_TRUE(b.engine->applySettingsJson(one));
    EXPECT_EQ(b.engine->settingsJson(), one);
    EXPECT_FALSE(b.engine->snapshot().strips[2].mute);
    EXPECT_TRUE(b.engine->snapshot().strips[3].effects.empty());
    b.run(2);
}
