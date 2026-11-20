#include <gtest/gtest.h>

#include <atomic>
#include <cstring>
#include <chrono>
#include <thread>

#include "plugins/manager.hpp"
#include "plugins/native_source.hpp"
#include "plugins/runtime.hpp"
#include "plugins_test_util.hpp"
#include "video/frame_ops.hpp"

using namespace hb;
using namespace hb::plugins;
using namespace hb::testutil;

namespace {
// ---- a configurable fake plugin (function pointers filled by the test) ----
struct Fake {
    static inline std::atomic<int> opened{0}, closed{0}, renders{0};
    static inline int mode = 0;   // 0 ok, 1 no signal, 2 error, 3 slow, 4 crash(throws)
    static int list(hb_source_desc* out, int max) {
        if (max < 5) return -1;
        std::memset(out, 0, sizeof(hb_source_desc) * 5);
        std::strcpy(out[0].id, "good");   std::strcpy(out[0].name, "Boa");
        std::strcpy(out[1].id, "bad id!"); std::strcpy(out[1].name, "Id inválido");     // skipped
        std::strcpy(out[2].id, "good");   std::strcpy(out[2].name, "Duplicada");        // skipped
        std::memset(out[3].id, 'x', HB_PLUGIN_STR_MAX);                                  // not NUL terminated
        std::strcpy(out[4].id, "ctl");    std::memcpy(out[4].name, "A\x01" "B\n", 5);   // control chars stripped
        return 5;
    }
    static void* open(const char* id) {
        if (std::string(id) == "nope") return nullptr;
        ++opened;
        return new int(7);
    }
    static int render(void*, uint8_t* bgra, int w, int h, int stride, uint64_t) {
        ++renders;
        if (mode == 1) return HB_RENDER_NO_SIGNAL;
        if (mode == 2) return -5;
        if (mode == 3) std::this_thread::sleep_for(std::chrono::milliseconds(15));
        if (mode == 4) throw std::runtime_error("plugin bug");
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                uint8_t* p = bgra + y * stride + x * 4;
                p[0] = 10; p[1] = 20; p[2] = 30; p[3] = 0;       // alpha left at 0 on purpose
            }
        return HB_RENDER_OK;
    }
    static void close(void* h) { ++closed; delete static_cast<int*>(h); }
    static hb_plugin_v1 api() {
        hb_plugin_v1 a{};
        a.struct_size = sizeof a;
        a.api_version = HB_PLUGIN_API_VERSION;
        a.list_sources = list; a.open_source = open; a.render = render; a.close_source = close;
        return a;
    }
    static void reset() { opened = closed = renders = 0; mode = 0; }
};

video::Frame frame(int w = 32, int h = 18) {
    video::Frame f;
    video::prepareFrame(f, w, h);
    return f;
}
}  // namespace

TEST(NativePlugin, RejectsBadApiTables) {
    std::string err;
    auto a = Fake::api();
    EXPECT_TRUE(NativePlugin::fromApi(a, "com.t.x", err));
    auto bad = a; bad.api_version = 99;
    EXPECT_FALSE(NativePlugin::fromApi(bad, "com.t.x", err));
    bad = a; bad.struct_size = 8;
    EXPECT_FALSE(NativePlugin::fromApi(bad, "com.t.x", err));
    for (int i = 0; i < 4; ++i) {
        bad = a;
        if (i == 0) bad.list_sources = nullptr;
        if (i == 1) bad.open_source = nullptr;
        if (i == 2) bad.render = nullptr;
        if (i == 3) bad.close_source = nullptr;
        EXPECT_FALSE(NativePlugin::fromApi(bad, "com.t.x", err)) << i;
    }
}

TEST(NativePlugin, ListSourcesSanitisesWhatThePluginReturns) {
    Fake::reset();
    std::string err;
    auto p = NativePlugin::fromApi(Fake::api(), "com.t.x", err);
    ASSERT_TRUE(p);
    std::vector<NativePlugin::SourceInfo> list;
    ASSERT_TRUE(p->listSources(list, err));
    // "good" (once), the 128-x id is longer than 64 -> dropped, "bad id!" dropped, "ctl" kept with a cleaned name
    ASSERT_EQ(list.size(), 2u);
    EXPECT_EQ(list[0].id, "good");
    EXPECT_EQ(list[0].name, "Boa");
    EXPECT_EQ(list[1].id, "ctl");
    EXPECT_EQ(list[1].name, "AB");
}

TEST(NativePlugin, RenderFillsFrameAndForcesOpaqueAlpha) {
    Fake::reset();
    std::string err;
    auto p = NativePlugin::fromApi(Fake::api(), "com.t.x", err);
    auto src = p->open("good", err);
    ASSERT_TRUE(src) << err;
    auto f = frame();
    src->render(f, 0);
    EXPECT_EQ(f.data[0], 10); EXPECT_EQ(f.data[1], 20); EXPECT_EQ(f.data[2], 30); EXPECT_EQ(f.data[3], 255);
    EXPECT_EQ(src->status().state, video::SourceStatus::State::Online);
    EXPECT_FALSE(p->open("nope", err));
    EXPECT_FALSE(p->open("not-listed", err));
    EXPECT_FALSE(p->open("../../etc", err));
}

TEST(NativePlugin, SourceIsClosedExactlyOnce) {
    Fake::reset();
    std::string err;
    auto p = NativePlugin::fromApi(Fake::api(), "com.t.x", err);
    {
        auto src = p->open("good", err);
        ASSERT_TRUE(src);
        EXPECT_EQ(Fake::opened.load(), 1);
        EXPECT_EQ(Fake::closed.load(), 0);
    }
    EXPECT_EQ(Fake::closed.load(), 1);
}

TEST(NativePlugin, NoSignalIsReportedAndHarmless) {
    Fake::reset();
    std::string err;
    auto p = NativePlugin::fromApi(Fake::api(), "com.t.x", err);
    auto src = p->open("good", err);
    Fake::mode = 1;
    auto f = frame();
    for (int i = 0; i < 100; ++i) src->render(f, static_cast<std::uint64_t>(i));    // never "faulted" just for being empty
    EXPECT_EQ(src->status().state, video::SourceStatus::State::NoSignal);
    Fake::mode = 0;
    src->render(f, 0);
    EXPECT_EQ(src->status().state, video::SourceStatus::State::Online);
}

TEST(NativePlugin, RepeatedErrorsSwitchTheSourceOffAndStopCallingIt) {
    Fake::reset();
    std::string err;
    auto p = NativePlugin::fromApi(Fake::api(), "com.t.x", err);
    SourceLimits lim;
    lim.maxErrorsInARow = 5;
    auto src = p->open("good", err, lim);
    Fake::mode = 2;
    auto f = frame();
    for (int i = 0; i < 20; ++i) src->render(f, 0);
    EXPECT_EQ(src->status().state, video::SourceStatus::State::Error);
    EXPECT_EQ(Fake::renders.load(), 5);                      // not called again after being disabled
    EXPECT_NE(f.data[0], 10);                                // dark placeholder, not stale/garbage
}

TEST(NativePlugin, SlowRenderIsDetectedAndSwitchedOff) {
    Fake::reset();
    std::string err;
    auto p = NativePlugin::fromApi(Fake::api(), "com.t.x", err);
    SourceLimits lim;
    lim.slowMs = 5;
    lim.maxSlowInARow = 3;
    auto src = p->open("good", err, lim);
    Fake::mode = 3;
    auto f = frame();
    for (int i = 0; i < 6; ++i) src->render(f, 0);
    EXPECT_EQ(src->status().state, video::SourceStatus::State::Error);
    EXPECT_EQ(Fake::renders.load(), 3);
}

TEST(NativePlugin, ExceptionFromThePluginIsContained) {
    Fake::reset();
    std::string err;
    auto p = NativePlugin::fromApi(Fake::api(), "com.t.x", err);
    auto src = p->open("good", err);
    Fake::mode = 4;
    auto f = frame();
    EXPECT_NO_THROW(src->render(f, 0));
    EXPECT_EQ(src->status().state, video::SourceStatus::State::Error);
    EXPECT_NO_THROW(src->render(f, 1));
}

TEST(NativePlugin, DegenerateFramesNeverReachThePlugin) {
    Fake::reset();
    std::string err;
    auto p = NativePlugin::fromApi(Fake::api(), "com.t.x", err);
    auto src = p->open("good", err);
    video::Frame empty;
    src->render(empty, 0);
    video::Frame lying;
    lying.width = 1000; lying.height = 1000; lying.stride = 4000;            // claims 4 MB, has none
    src->render(lying, 0);
    EXPECT_EQ(Fake::renders.load(), 0);
}

TEST(NativePlugin, ConcurrentRenderIsSerialised) {
    Fake::reset();
    std::string err;
    auto p = NativePlugin::fromApi(Fake::api(), "com.t.x", err);
    auto src = p->open("good", err);
    std::vector<std::thread> ts;
    for (int t = 0; t < 4; ++t)
        ts.emplace_back([&] {
            auto f = frame();
            for (int i = 0; i < 200; ++i) src->render(f, static_cast<std::uint64_t>(i));
        });
    for (auto& t : ts) t.join();
    EXPECT_EQ(Fake::renders.load(), 800);
}

// ---------------------------------------------------------------------------------------------------
// The real example plugin, loaded from a real shared library through the real loader.
// ---------------------------------------------------------------------------------------------------
TEST(NativeExample, LoadsListsOpensAndRenders) {
    std::string err;
    auto p = NativePlugin::load(HB_EXAMPLE_PLUGIN_PATH, "com.hbstudio.example.sources", err);
    ASSERT_TRUE(p) << err;
    std::vector<NativePlugin::SourceInfo> list;
    ASSERT_TRUE(p->listSources(list, err));
    ASSERT_EQ(list.size(), 2u);
    EXPECT_EQ(list[0].id, "bars");

    auto bars = p->open("bars", err);
    ASSERT_TRUE(bars) << err;
    auto f = frame(70, 20);
    bars->render(f, 0);
    EXPECT_EQ(f.data[2], 235);                               // first bar: white (R)
    EXPECT_EQ(f.data[(69) * 4 + 0], 235);                    // last bar: blue (B)
    EXPECT_EQ(f.data[(69) * 4 + 2], 16);
    EXPECT_EQ(f.data[3], 255);
    auto ramp = p->open("ramp", err);
    ASSERT_TRUE(ramp);
    auto g = frame(70, 20);
    ramp->render(g, 0);
    EXPECT_NE(f.data, g.data);
    EXPECT_EQ(bars->status().width, 1280);
}

TEST(NativeExample, MissingOrNonPluginLibrariesFailCleanly) {
    std::string err;
    EXPECT_FALSE(NativePlugin::load("/definitely/not/here.so", "com.t.x", err));
    EXPECT_FALSE(err.empty());
    const auto dir = makeTempDir("nonlib");
    writeFile(dir / "junk.so", std::vector<std::uint8_t>(100, 0x41));
    EXPECT_FALSE(NativePlugin::load(pathToUtf8(dir / "junk.so"), "com.t.x", err));
    std::filesystem::remove_all(dir);
}

TEST(NativeExample, FullFlowPackageInstallEnableLoadOpen) {
    namespace fs = std::filesystem;
    const std::string libPath = HB_EXAMPLE_PLUGIN_PATH;
    const std::string libName = pathToUtf8(pathFromUtf8(libPath).filename());
    auto libBytes = readFile(libPath);
    ASSERT_FALSE(libBytes.empty());
    const std::string libData(libBytes.begin(), libBytes.end());

    const auto work = makeTempDir("flow");
    PluginManager mgr(pathToUtf8(work / "plugins"), platformKey());
    mgr.scan();
    const std::string pkg = writeFile(work / "ex.hbplugin",
        makeZip({{"manifest.json", sourceManifest("com.hbstudio.example.sources", libName, libData)}, {libName, libData}}));
    auto rv = mgr.inspect(pkg);
    ASSERT_TRUE(rv.ok) << rv.error;
    ASSERT_TRUE(mgr.install(pkg, rv.info.packageSha256).ok);

    PluginRuntime rt(mgr);
    rt.sync();
    EXPECT_TRUE(rt.sources().empty());                       // installed but not enabled: nothing is loaded

    ASSERT_TRUE(mgr.setEnabled("com.hbstudio.example.sources", true).ok);
    rt.sync();
    auto src = rt.sources();
    ASSERT_EQ(src.size(), 2u);
    EXPECT_EQ(src[0].fullId, "plugin:com.hbstudio.example.sources:bars");
    InstalledPlugin ip;
    ASSERT_TRUE(mgr.find("com.hbstudio.example.sources", ip));
    EXPECT_TRUE(ip.loaded);

    std::string err;
    auto live = rt.open(src[0].fullId, err);
    ASSERT_TRUE(live) << err;
    auto f = frame();
    live->render(f, 0);
    EXPECT_EQ(f.data[3], 255);

    // disabling hides the sources immediately
    mgr.setEnabled("com.hbstudio.example.sources", false);
    EXPECT_TRUE(rt.sources().empty());
    EXPECT_FALSE(rt.open(src[0].fullId, err));

    std::string pid, sid;
    EXPECT_TRUE(PluginRuntime::parseFullId("plugin:com.a.b:src-1", pid, sid));
    EXPECT_EQ(pid, "com.a.b");
    EXPECT_EQ(sid, "src-1");
    for (const char* bad : {"", "plugin:", "plugin:x", "plugin:com.a.b:", "plugin:com.a.b:a/b", "dshow:x", "plugin:Con:x", "plugin:..:x"})
        EXPECT_FALSE(PluginRuntime::parseFullId(bad, pid, sid)) << bad;
    { std::error_code ec; fs::remove_all(work, ec); }   // best-effort: on Windows a loaded plugin library stays locked until the process exits (by design, see docs/PLUGINS.md)
}

TEST(NativeExample, TamperedLibraryIsNeverLoadedAtStartup) {
    namespace fs = std::filesystem;
    const std::string libPath = HB_EXAMPLE_PLUGIN_PATH;
    const std::string libName = pathToUtf8(pathFromUtf8(libPath).filename());
    auto libBytes = readFile(libPath);
    const std::string libData(libBytes.begin(), libBytes.end());
    const auto work = makeTempDir("tamper");
    PluginManager mgr(pathToUtf8(work / "plugins"), platformKey());
    mgr.scan();
    const std::string pkg = writeFile(work / "ex.hbplugin",
        makeZip({{"manifest.json", sourceManifest("com.hbstudio.example.sources", libName, libData)}, {libName, libData}}));
    ASSERT_TRUE(mgr.install(pkg, mgr.inspect(pkg).info.packageSha256).ok);
    ASSERT_TRUE(mgr.setEnabled("com.hbstudio.example.sources", true).ok);
    {   // attacker modifies the library between sessions
        std::ofstream f(work / "plugins" / "com.hbstudio.example.sources" / pathFromUtf8(libName), std::ios::binary | std::ios::trunc);
        f << "NOT THE ORIGINAL";
    }
    PluginManager next(pathToUtf8(work / "plugins"), platformKey());
    next.scan();
    PluginRuntime rt(next);
    rt.sync();
    EXPECT_TRUE(rt.sources().empty());
    InstalledPlugin ip;
    ASSERT_TRUE(next.find("com.hbstudio.example.sources", ip));
    EXPECT_FALSE(ip.loaded);
    EXPECT_FALSE(ip.enabled);                                 // switched off with an error shown to the user
    EXPECT_FALSE(ip.error.empty());
    { std::error_code ec; fs::remove_all(work, ec); }   // best-effort: on Windows a loaded plugin library stays locked until the process exits (by design, see docs/PLUGINS.md)
}
