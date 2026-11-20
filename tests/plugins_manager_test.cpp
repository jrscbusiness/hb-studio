#include <gtest/gtest.h>

#include "plugins/manager.hpp"
#include "plugins_test_util.hpp"

using namespace hb;
using namespace hb::plugins;
using namespace hb::testutil;
namespace fs = std::filesystem;

namespace {
struct Env {
    fs::path root = makeTempDir("mgr");
    fs::path pkgDir = makeTempDir("pkg");
    PluginManager mgr{pathToUtf8(root / "plugins"), platformKey()};
    Env() { mgr.scan(); }
    ~Env() {
        std::error_code ec;
        fs::remove_all(root, ec);
        fs::remove_all(pkgDir, ec);
    }
    std::string pkg(const std::string& name, const Entries& e) { return writeFile(pkgDir / name, makeZip(e)); }
    static std::string theme(const std::string& id, const char* bg = "#0A1226") {
        return R"({"schema":1,"id":")" + id + R"(","name":"Tema )" + id + R"(","version":"1.0.0","type":"theme","theme":{"colorScheme":"dark","vars":{"--bg":")" + bg + R"("}}})";
    }
    // installs with the exact sha that review showed, like the app does
    Result install(const std::string& path) {
        auto rv = mgr.inspect(path);
        if (!rv.ok) return {false, rv.error};
        return mgr.install(path, rv.info.packageSha256);
    }
};
}  // namespace

TEST(Manager, InstallStartsDisabledThenEnableDisable) {
    Env e;
    auto r = e.install(e.pkg("a.hbplugin", {{"manifest.json", Env::theme("com.test.a")}}));
    ASSERT_TRUE(r.ok) << r.message;
    InstalledPlugin p;
    ASSERT_TRUE(e.mgr.find("com.test.a", p));
    EXPECT_FALSE(p.enabled);
    EXPECT_TRUE(e.mgr.activeThemeVars().empty());

    ASSERT_TRUE(e.mgr.setEnabled("com.test.a", true).ok);
    EXPECT_EQ(e.mgr.activeThemeVars().at("--bg"), "#0A1226");
    EXPECT_EQ(e.mgr.activeColorScheme(), "dark");
    ASSERT_TRUE(e.mgr.setEnabled("com.test.a", false).ok);
    EXPECT_TRUE(e.mgr.activeThemeVars().empty());
    EXPECT_FALSE(e.mgr.setEnabled("com.test.missing", true).ok);
}

TEST(Manager, OnlyOneThemeIsActive) {
    Env e;
    ASSERT_TRUE(e.install(e.pkg("a.hbplugin", {{"manifest.json", Env::theme("com.test.a", "#111111")}})).ok);
    ASSERT_TRUE(e.install(e.pkg("b.hbplugin", {{"manifest.json", Env::theme("com.test.b", "#222222")}})).ok);
    e.mgr.setEnabled("com.test.a", true);
    e.mgr.setEnabled("com.test.b", true);
    EXPECT_EQ(e.mgr.activeThemeId(), "com.test.b");
    InstalledPlugin a;
    ASSERT_TRUE(e.mgr.find("com.test.a", a));
    EXPECT_FALSE(a.enabled);
}

TEST(Manager, StatePersistsAcrossRestarts) {
    Env e;
    ASSERT_TRUE(e.install(e.pkg("a.hbplugin", {{"manifest.json", Env::theme("com.test.a")}})).ok);
    e.mgr.setEnabled("com.test.a", true);
    PluginManager again(pathToUtf8(e.root / "plugins"), platformKey());
    again.scan();
    EXPECT_EQ(again.activeThemeId(), "com.test.a");
    ASSERT_EQ(again.list().size(), 1u);
}

TEST(Manager, UpdateReplacesFilesAndResetsEnabled) {
    Env e;
    ASSERT_TRUE(e.install(e.pkg("a1.hbplugin", {{"manifest.json", Env::theme("com.test.a", "#111111")}})).ok);
    e.mgr.setEnabled("com.test.a", true);
    auto rv = e.mgr.inspect(e.pkg("a2.hbplugin", {{"manifest.json", Env::theme("com.test.a", "#333333")}, {"notes.txt", "v2"}}));
    ASSERT_TRUE(rv.ok);
    EXPECT_TRUE(rv.replaces);
    EXPECT_EQ(rv.installedVersion, "1.0.0");
    ASSERT_TRUE(e.mgr.install(e.pkgDir.string() + "/a2.hbplugin", rv.info.packageSha256).ok);
    InstalledPlugin p;
    ASSERT_TRUE(e.mgr.find("com.test.a", p));
    EXPECT_FALSE(p.enabled);                                   // new code never runs without consent
    EXPECT_EQ(p.manifest.themeVars.at("--bg"), "#333333");
    EXPECT_TRUE(fs::exists(fs::path(p.dir) / "notes.txt"));
}

TEST(Manager, InstallRefusesAPackageThatChangedAfterReview) {
    Env e;
    const std::string path = e.pkg("a.hbplugin", {{"manifest.json", Env::theme("com.test.a")}});
    auto rv = e.mgr.inspect(path);
    ASSERT_TRUE(rv.ok);
    writeFile(e.pkgDir / "a.hbplugin", makeZip({{"manifest.json", Env::theme("com.test.a", "#999999")}}));   // swapped on disk
    auto r = e.mgr.install(path, rv.info.packageSha256);
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(e.mgr.list().empty());
    EXPECT_FALSE(fs::exists(e.root / "plugins" / "com.test.a"));
}

TEST(Manager, HostilePackagesInstallNothing) {
    Env e;
    for (const char* name : {"../evil.txt", "con.txt", "a\\b", "x/../../y"}) {
        const std::string path = e.pkg("bad.hbplugin", {{"manifest.json", Env::theme("com.test.bad")}, {name, "x"}});
        EXPECT_FALSE(e.install(path).ok) << name;
    }
    EXPECT_TRUE(e.mgr.list().empty());
    EXPECT_FALSE(fs::exists(e.root / "evil.txt"));
    EXPECT_FALSE(fs::exists(e.root / "plugins" / "evil.txt"));
}

TEST(Manager, AbsolutePathEntryCannotEscapeTheInstallFolder) {
    Env e;
    auto zip = makeZipRawName({{"manifest.json", Env::theme("com.test.abs")}, {"Xescape.txt", "pwned"}}, "Xescape.txt", "/escape.txt");
    const std::string path = writeFile(e.pkgDir / "abs.hbplugin", zip);
    EXPECT_FALSE(e.install(path).ok);
    EXPECT_TRUE(e.mgr.list().empty());
}

TEST(Manager, RemoveDeletesFilesAndSettings) {
    Env e;
    ASSERT_TRUE(e.install(e.pkg("a.hbplugin", {{"manifest.json", Env::theme("com.test.a")}})).ok);
    e.mgr.setEnabled("com.test.a", true);
    ASSERT_TRUE(e.mgr.remove("com.test.a").ok);
    EXPECT_TRUE(e.mgr.list().empty());
    EXPECT_FALSE(fs::exists(e.root / "plugins" / "com.test.a"));
    EXPECT_TRUE(e.mgr.activeThemeVars().empty());
    EXPECT_FALSE(e.mgr.remove("com.test.a").ok);
}

TEST(Manager, LoadedLibraryIsRemovedOnlyAtNextStart) {
    Env e;
    const std::string lib = "LIB";
    ASSERT_TRUE(e.install(e.pkg("s.hbplugin", {{"manifest.json", sourceManifest("com.test.s", "libs.so", lib)}, {"libs.so", lib}})).ok);
    ASSERT_TRUE(e.mgr.setEnabled("com.test.s", true).ok);
    e.mgr.markLoaded("com.test.s", true);                       // as if the app had dlopen()ed it
    auto r = e.mgr.remove("com.test.s");
    ASSERT_TRUE(r.ok);
    EXPECT_NE(r.message.find("reiniciar"), std::string::npos);
    EXPECT_TRUE(fs::exists(e.root / "plugins" / "com.test.s"));
    PluginManager next(pathToUtf8(e.root / "plugins"), platformKey());
    next.scan();                                                 // "restart"
    EXPECT_TRUE(next.list().empty());
    EXPECT_FALSE(fs::exists(e.root / "plugins" / "com.test.s"));
}

TEST(Manager, CannotUpdateAPluginWhileItsLibraryIsLoaded) {
    Env e;
    const std::string lib = "LIB";
    ASSERT_TRUE(e.install(e.pkg("s.hbplugin", {{"manifest.json", sourceManifest("com.test.s", "libs.so", lib)}, {"libs.so", lib}})).ok);
    e.mgr.setEnabled("com.test.s", true);
    e.mgr.markLoaded("com.test.s", true);
    auto r = e.install(e.pkg("s2.hbplugin", {{"manifest.json", sourceManifest("com.test.s", "libs.so", "LIB2")}, {"libs.so", "LIB2"}}));
    EXPECT_FALSE(r.ok);
}

TEST(Manager, NativeLibraryIsReHashedBeforeEnableAndBeforeLoad) {
    Env e;
    const std::string lib = "ORIGINAL-LIB";
    ASSERT_TRUE(e.install(e.pkg("s.hbplugin", {{"manifest.json", sourceManifest("com.test.s", "libs.so", lib)}, {"libs.so", lib}})).ok);
    const fs::path libPath = e.root / "plugins" / "com.test.s" / "libs.so";

    std::string path, err;
    ASSERT_TRUE(e.mgr.setEnabled("com.test.s", true).ok);
    ASSERT_TRUE(e.mgr.verifiedLibraryPath("com.test.s", path, err)) << err;

    {   // someone swaps the DLL on disk after installation
        std::ofstream f(libPath, std::ios::binary | std::ios::trunc);
        f << "MALICIOUS";
    }
    EXPECT_FALSE(e.mgr.verifiedLibraryPath("com.test.s", path, err));
    EXPECT_NE(err.find("sha256"), std::string::npos);
    ASSERT_TRUE(e.mgr.setEnabled("com.test.s", false).ok);
    EXPECT_FALSE(e.mgr.setEnabled("com.test.s", true).ok);       // tampered: enabling refused
}

TEST(Manager, RuntimeFailureSwitchesThePluginOff) {
    Env e;
    ASSERT_TRUE(e.install(e.pkg("a.hbplugin", {{"manifest.json", Env::theme("com.test.a")}})).ok);
    e.mgr.setEnabled("com.test.a", true);
    e.mgr.setRuntimeError("com.test.a", "falhou");
    InstalledPlugin p;
    ASSERT_TRUE(e.mgr.find("com.test.a", p));
    EXPECT_FALSE(p.enabled);
    EXPECT_EQ(p.error, "falhou");
}

TEST(Manager, ScanToleratesGarbageInThePluginsFolder) {
    Env e;
    const fs::path root = e.root / "plugins";
    fs::create_directories(root / "no-manifest");
    fs::create_directories(root / "bad-json");
    std::ofstream(root / "bad-json" / "manifest.json") << "{oops";
    fs::create_directories(root / "com.test.mismatch");
    std::ofstream(root / "com.test.mismatch" / "manifest.json") << Env::theme("com.other.id");
    fs::create_directories(root / ".hidden");
    std::ofstream(root / "loose-file.txt") << "x";
    std::ofstream(root / "state.json") << "\xFF\xFE garbage";
    e.mgr.scan();
    auto list = e.mgr.list();
    EXPECT_EQ(list.size(), 3u);
    for (const auto& p : list) {
        EXPECT_FALSE(p.valid) << p.manifest.id;
        EXPECT_FALSE(p.error.empty());
        EXPECT_FALSE(e.mgr.setEnabled(p.manifest.id, true).ok);      // invalid plugins cannot be enabled
        EXPECT_TRUE(e.mgr.remove(p.manifest.id).ok);                 // but can be removed
    }
}

TEST(Manager, UnsupportedPlatformIsRefusedAtReview) {
    Env e;
    const std::string m = R"({"schema":1,"id":"com.test.mac","name":"Mac only","version":"1.0.0","type":"source","apiVersion":1,
        "library":{"macos-arm64":"libm.dylib"},"sha256":{"libm.dylib":")" + core::Sha256::hex("x") + R"("}})";
    auto rv = e.mgr.inspect(e.pkg("m.hbplugin", {{"manifest.json", m}, {"libm.dylib", "x"}}));
    EXPECT_FALSE(rv.ok);
    EXPECT_NE(rv.error.find(platformKey()), std::string::npos);
}
