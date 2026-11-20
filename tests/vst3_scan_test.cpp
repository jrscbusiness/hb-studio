// scanPlugins()/typeIdFor() against the project-authored test fixture plugin (see
// plugins/examples/vst3-test-fixture/README.md) — the only VST3 module available in this
// environment. HB_VST3_FIXTURE_PACKAGE_PATH is the fixture's own ".../HB VST3 Test Fixture.vst3"
// bundle path, defined by cmake/modules/vst3.cmake from the build.
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "vst3/vst3.hpp"

using hb::vst3::scanPlugins;
using hb::vst3::typeIdFor;
using hb::vst3::Vst3PluginInfo;

namespace {
std::string fixtureFolder() {
    return std::filesystem::path(HB_VST3_FIXTURE_PACKAGE_PATH).parent_path().string();
}
}  // namespace

TEST(Vst3Scan, FindsTheFixturePlugin) {
    const auto found = scanPlugins({fixtureFolder()});
    ASSERT_EQ(found.size(), 1u) << "expected exactly the fixture's one kVstAudioEffectClass class "
                                   "(its controller class is a different category and must not be listed)";
    const Vst3PluginInfo& info = found.front();
    EXPECT_EQ(info.name, "HB Test Gain (fixture)");
    EXPECT_EQ(info.vendor, "HB Studio (test fixture)");
    EXPECT_EQ(info.category, "Fx");
    EXPECT_EQ(info.path, HB_VST3_FIXTURE_PACKAGE_PATH);
    EXPECT_EQ(info.classId.size(), 32u);
    for (char c : info.classId) EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')) << "not lowercase hex";
}

TEST(Vst3Scan, TypeIdIsStableAcrossScans) {
    const auto first = scanPlugins({fixtureFolder()});
    const auto second = scanPlugins({fixtureFolder()});
    ASSERT_EQ(first.size(), 1u);
    ASSERT_EQ(second.size(), 1u);
    EXPECT_EQ(typeIdFor(first.front()), typeIdFor(second.front()));
    EXPECT_EQ(typeIdFor(first.front()), "vst3:" + first.front().classId);
}

TEST(Vst3Scan, IgnoresAFolderThatDoesNotExist) {
    EXPECT_TRUE(scanPlugins({"/this/folder/does/not/exist/hb-vst3-test"}).empty());
}

TEST(Vst3Scan, SkipsAFileThatIsNotReallyAVst3Module) {
    const auto dir = std::filesystem::temp_directory_path() / "hb_vst3_scan_test_bogus";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    {
        std::ofstream f((dir / "NotAPlugin.vst3").string(), std::ios::binary);
        f << "this is not a shared library";
    }
    EXPECT_TRUE(scanPlugins({dir.string()}).empty());
    std::filesystem::remove_all(dir);
}

TEST(Vst3Scan, ExtraFoldersAreAdditiveToTheStandardOnes) {
    // On this (non-Windows) platform %COMMONPROGRAMFILES%/%LOCALAPPDATA% simply don't resolve to
    // anything, so the standard folders contribute nothing — scanPlugins() must still find what's
    // in extraFolders (this is how the real app lets the operator add extra install locations).
    const auto found = scanPlugins({fixtureFolder()});
    EXPECT_EQ(found.size(), 1u);
}
