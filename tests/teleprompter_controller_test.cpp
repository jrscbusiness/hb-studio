#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "core/json.hpp"
#include "teleprompter/controller.hpp"

using namespace hb::teleprompter;
namespace fs = std::filesystem;
namespace json = hb::core::json;

namespace {
struct Fx {
    fs::path dir;
    ManualClock clock;
    Controller c;
    static fs::path mk() {
        static int n = 0;
        // ctest runs each test in its own process (n and random_seed repeat), so the TEST NAME must be part
        // of the directory or parallel runs delete each other's files.
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        auto p = fs::temp_directory_path() / ("hb_tp_ctl_" + std::string(info ? info->name() : "x") + "_" +
                                              std::to_string(++n));
        fs::remove_all(p);
        return p;
    }
    Fx() : dir(mk()), c(dir, &clock) { c.load(); }
    ~Fx() { std::error_code ec; fs::remove_all(dir, ec); }
    json::Value parse(const std::string& s) {
        json::Value v;
        EXPECT_TRUE(json::parse(s, v)) << s;
        return v;
    }
};
}  // namespace

TEST(TpController, FirstRunCreatesSampleScript) {
    Fx f;
    const auto v = f.parse(f.c.scriptJson());
    EXPECT_EQ(v.get("t")->s, "tpscript");
    EXPECT_GT(v.get("words")->n, 20);
    bool hasCue = false;
    for (const auto& b : v.get("blocks")->a) hasCue |= b.get("k")->s == "c";
    EXPECT_TRUE(hasCue);
    EXPECT_EQ(f.parse(f.c.listJson()).get("scripts")->a.size(), 1u);
}

TEST(TpController, IgnoresForeignMessages) {
    Fx f;
    EXPECT_FALSE(f.c.execute("cut").handled);
    EXPECT_FALSE(f.c.execute("tpx|play").handled);
    EXPECT_FALSE(f.c.execute("").handled);
    EXPECT_TRUE(f.c.execute("tp|play").handled);
}

TEST(TpController, RemoteCommandsPlayPauseSpeedSeek) {
    Fx f;
    f.c.execute("tp|countdown|0");
    f.c.execute("tp|layout|p|5000|800");
    auto fx = f.c.execute("tp|speed|200");
    EXPECT_TRUE(fx.state);
    EXPECT_EQ(f.c.snapshot().wpm, 200);
    f.c.execute("tp|play");
    EXPECT_EQ(f.c.snapshot().state, RunState::Playing);
    f.clock.advance(10);
    EXPECT_GT(f.c.snapshot().pos, 0.0);
    f.c.execute("tp|pause");
    EXPECT_EQ(f.c.snapshot().state, RunState::Paused);
    f.c.execute("tp|seek|0.5");
    EXPECT_DOUBLE_EQ(f.c.snapshot().pos, 0.5);
    f.c.execute("tp|nudge|-10");
    EXPECT_EQ(f.c.snapshot().wpm, 190);
    f.c.execute("tp|restart");
    EXPECT_EQ(f.c.snapshot().pos, 0.0);
}

TEST(TpController, BadArgumentsAreRejectedWithoutEffect) {
    Fx f;
    f.c.execute("tp|speed|abc");
    f.c.execute("tp|speed|");
    f.c.execute("tp|seek|nan");
    f.c.execute("tp|speed|1e999");
    f.c.execute("tp|mode|bogus");
    f.c.execute("tp|style|fg|red");
    f.c.execute("tp|style|fg|#12345");
    f.c.execute("tp|style|unknown|1");
    f.c.execute("tp|layout|x|1|2");
    f.c.execute("tp|");
    f.c.execute("tp");
    f.c.execute("tp|select|../etc");
    EXPECT_EQ(f.c.snapshot().wpm, 150);
    EXPECT_EQ(f.c.style().fg, "#FFFFFF");
}

TEST(TpController, SpeedIsClamped) {
    Fx f;
    f.c.execute("tp|speed|100000");
    EXPECT_EQ(f.c.snapshot().wpm, kMaxWpm);
    f.c.execute("tp|speed|-3");
    EXPECT_EQ(f.c.snapshot().wpm, kMinWpm);
}

TEST(TpController, StyleCommandsAndJson) {
    Fx f;
    f.c.execute("tp|style|font|9999");
    f.c.execute("tp|style|bg|#102030");
    f.c.execute("tp|style|mirror|1");
    f.c.execute("tp|style|flip|1");
    f.c.execute("tp|style|marker|0");
    f.c.execute("tp|style|markerpos|50");
    f.c.execute("tp|style|margin|12");
    f.c.execute("tp|style|lh|1.6");
    const auto v = f.parse(f.c.styleJson());
    EXPECT_EQ(v.get("font")->n, 220);
    EXPECT_EQ(v.get("bg")->s, "#102030");
    EXPECT_TRUE(v.get("mirror")->b);
    EXPECT_TRUE(v.get("flip")->b);
    EXPECT_FALSE(v.get("marker")->b);
    EXPECT_EQ(v.get("markerpos")->n, 50);
    EXPECT_EQ(v.get("margin")->n, 12);
    EXPECT_NEAR(v.get("lh")->n, 1.6, 1e-9);
}

TEST(TpController, SettingsPersistAcrossRestart) {
    Fx f;
    f.c.execute("tp|style|fg|#FFFF00");
    f.c.execute("tp|speed|175");
    f.c.execute("tp|countdown|5");
    f.c.execute("tp|mode|pxs");
    f.c.flush();
    Controller again(f.dir, &f.clock);
    again.load();
    EXPECT_EQ(again.style().fg, "#FFFF00");
    const auto s = again.snapshot();
    EXPECT_EQ(s.countdownSec, 5);
    EXPECT_EQ(s.mode, SpeedMode::PxPerSec);
}

TEST(TpController, ScriptLifecycle) {
    Fx f;
    f.c.execute("tp|new|Meu roteiro");
    auto v = f.parse(f.c.listJson());
    ASSERT_EQ(v.get("scripts")->a.size(), 2u);
    const std::string id = v.get("current")->s;
    f.c.execute("tp|save|" + id + "|um dois tres\n\n# nota | com barra\n\nquatro cinco");
    const auto sc = f.parse(f.c.scriptJson());
    EXPECT_EQ(sc.get("words")->n, 5);
    EXPECT_EQ(sc.get("blocks")->a.size(), 3u);
    EXPECT_EQ(sc.get("blocks")->a[1].get("t")->s, "nota | com barra");   // '|' inside text survives
    EXPECT_NEAR(sc.get("blocks")->a[2].get("at")->n, 0.6, 1e-6);
    EXPECT_EQ(f.c.snapshot().words, 5);
    f.c.execute("tp|rename|" + id + "|Outro nome");
    EXPECT_EQ(f.parse(f.c.scriptJson()).get("name")->s, "Outro nome");
    f.c.execute("tp|delete|" + id);
    v = f.parse(f.c.listJson());
    EXPECT_EQ(v.get("scripts")->a.size(), 1u);
    EXPECT_NE(v.get("current")->s, id);
}

TEST(TpController, DeletingTheLastScriptLeavesAnEmptyOne) {
    Fx f;
    const auto cur = f.parse(f.c.listJson()).get("current")->s;
    f.c.execute("tp|delete|" + cur);
    const auto v = f.parse(f.c.listJson());
    ASSERT_EQ(v.get("scripts")->a.size(), 1u);
    EXPECT_EQ(f.parse(f.c.scriptJson()).get("words")->n, 0);
}

TEST(TpController, ImportSelectsNewScript) {
    Fx f;
    auto fx = f.c.execute("tp|import|De arquivo|linha com a\xE7\xE3o\r\nfim");
    EXPECT_TRUE(fx.list && fx.script);
    const auto sc = f.parse(f.c.scriptJson());
    EXPECT_EQ(sc.get("name")->s, "De arquivo");
    EXPECT_EQ(sc.get("text")->s, "linha com ação\nfim");
}

TEST(TpController, SaveToNonCurrentScriptDoesNotTouchCurrent) {
    Fx f;
    f.c.execute("tp|new|B");
    const auto idB = f.parse(f.c.listJson()).get("current")->s;
    f.c.execute("tp|new|C");
    const auto fx = f.c.execute("tp|save|" + idB + "|so no B");
    EXPECT_FALSE(fx.script);
    EXPECT_NE(f.parse(f.c.scriptJson()).get("text")->s, "so no B");
}

TEST(TpController, PollEmitsOnTransitionAndPeriodicResync) {
    Fx f;
    f.c.execute("tp|countdown|0");
    f.c.execute("tp|layout|p|5000|800");
    EXPECT_TRUE(f.c.poll().has_value());   // first poll always publishes
    EXPECT_FALSE(f.c.poll().has_value());
    f.c.execute("tp|play");
    EXPECT_TRUE(f.c.poll().has_value());   // state changed
    EXPECT_FALSE(f.c.poll().has_value());
    f.clock.advance(0.3);
    EXPECT_FALSE(f.c.poll().has_value());
    f.clock.advance(0.3);
    const auto j = f.c.poll();             // >= 0.5 s while moving: re-sync
    ASSERT_TRUE(j.has_value());
    const auto v = f.parse(*j);
    EXPECT_EQ(v.get("state")->s, "playing");
    EXPECT_GT(v.get("rate")->n, 0);
    EXPECT_GT(v.get("pos")->n, 0);
    f.clock.advance(100000);               // runs off the end -> Finished is announced
    const auto fin = f.c.poll();
    ASSERT_TRUE(fin.has_value());
    EXPECT_EQ(f.parse(*fin).get("state")->s, "finished");
}

TEST(TpController, PresenterLayoutWinsOverOperatorPreview) {
    Fx f;
    f.c.setPresenterOpen(true);
    f.c.execute("tp|layout|p|9000|1000");
    f.c.execute("tp|layout|o|3000|400");   // ignored while the presenter has reported
    EXPECT_EQ(f.c.snapshot().contentPx, 9000);
    f.c.setPresenterOpen(false);
    f.c.execute("tp|layout|o|3000|400");   // accepted again
    EXPECT_EQ(f.c.snapshot().contentPx, 3000);
}

TEST(TpController, PresenterCommandsAreReportedToTheHost) {
    Fx f;
    EXPECT_EQ(f.c.execute("tp|presenter|open").presenter, Effects::Presenter::Open);
    EXPECT_EQ(f.c.execute("tp|presenter|close").presenter, Effects::Presenter::Close);
    EXPECT_EQ(f.c.execute("tp|presenter|fullscreen").presenter, Effects::Presenter::Fullscreen);
    EXPECT_EQ(f.c.execute("tp|presenter|x").presenter, Effects::Presenter::None);
}

TEST(TpController, HelloResendsEverything) {
    Fx f;
    const auto fx = f.c.execute("tp|hello");
    EXPECT_TRUE(fx.state && fx.list && fx.script && fx.style);
}

TEST(TpController, StateJsonShape) {
    Fx f;
    const auto v = f.parse(f.c.stateJson());
    for (const char* k : {"seq", "state", "pos", "rate", "cd", "cdSec", "remain", "total", "mode", "wpm", "pxs", "words", "script", "presenter"})
        EXPECT_NE(v.get(k), nullptr) << k;
    EXPECT_EQ(v.get("state")->s, "stopped");
}
