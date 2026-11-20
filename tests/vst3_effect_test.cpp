// createEffect()/Effect processing against the project-authored test fixture plugin (fixed -6 dB
// gain, see plugins/examples/vst3-test-fixture/README.md).
#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

#include "vst3/vst3.hpp"

using hb::vst3::createEffect;
using hb::vst3::scanPlugins;
using hb::vst3::typeIdFor;
using hb::vst3::Vst3PluginInfo;

namespace {

Vst3PluginInfo fixtureInfo() {
    const auto found = scanPlugins({std::filesystem::path(HB_VST3_FIXTURE_PACKAGE_PATH).parent_path().string()});
    return found.empty() ? Vst3PluginInfo{} : found.front();
}

}  // namespace

TEST(Vst3Effect, CreateEffectSucceedsForTheFixture) {
    const auto info = fixtureInfo();
    ASSERT_FALSE(info.path.empty());
    std::string error;
    auto effect = createEffect(info, error);
    ASSERT_TRUE(effect) << "error: " << error;
    EXPECT_TRUE(error.empty());
    EXPECT_STREQ(effect->typeId(), typeIdFor(info).c_str());
    EXPECT_TRUE(effect->params().empty()) << "VST3 effects must always report empty params()";
}

TEST(Vst3Effect, CreateEffectFailsForAMissingModule) {
    Vst3PluginInfo bogus;
    bogus.path = "/this/does/not/exist.vst3";
    bogus.classId = std::string(32, '0');
    std::string error;
    auto effect = createEffect(bogus, error);
    EXPECT_FALSE(effect);
    EXPECT_FALSE(error.empty());
}

TEST(Vst3Effect, CreateEffectFailsForAnUnknownClassId) {
    auto info = fixtureInfo();
    ASSERT_FALSE(info.path.empty());
    info.classId = std::string(32, 'f');  // well-formed but not a class this module has
    std::string error;
    auto effect = createEffect(info, error);
    EXPECT_FALSE(effect);
    EXPECT_FALSE(error.empty());
}

TEST(Vst3Effect, AppliesTheFixedGainInPlaceStereo) {
    const auto info = fixtureInfo();
    ASSERT_FALSE(info.path.empty());
    std::string error;
    auto effect = createEffect(info, error);
    ASSERT_TRUE(effect) << error;

    effect->prepare(48000, 2);
    const std::size_t frames = 256;
    std::vector<float> buf(frames * 2);
    for (std::size_t i = 0; i < buf.size(); ++i) buf[i] = 1.0f;

    effect->process(buf.data(), frames);

    for (float v : buf) EXPECT_NEAR(v, 0.5f, 1e-5f);
}

TEST(Vst3Effect, AdaptsToAMonoRequest) {
    const auto info = fixtureInfo();
    ASSERT_FALSE(info.path.empty());
    std::string error;
    auto effect = createEffect(info, error);
    ASSERT_TRUE(effect) << error;

    effect->prepare(44100, 1);
    const std::size_t frames = 64;
    std::vector<float> buf(frames, 2.0f);
    effect->process(buf.data(), frames);
    for (float v : buf) EXPECT_NEAR(v, 1.0f, 1e-5f);
}

TEST(Vst3Effect, ProcessesMoreFramesThanTheInternalChunkSizeInOneCall) {
    const auto info = fixtureInfo();
    ASSERT_FALSE(info.path.empty());
    std::string error;
    auto effect = createEffect(info, error);
    ASSERT_TRUE(effect) << error;

    effect->prepare(48000, 2);
    // Vst3Effect::kMaxBlockFrames is 4096: exercise the multi-chunk loop in one process() call.
    const std::size_t frames = 9000;
    std::vector<float> buf(frames * 2, 1.0f);
    effect->process(buf.data(), frames);
    for (float v : buf) EXPECT_NEAR(v, 0.5f, 1e-5f);
}

TEST(Vst3Effect, ResetDoesNotCrashAndKeepsProcessing) {
    const auto info = fixtureInfo();
    ASSERT_FALSE(info.path.empty());
    std::string error;
    auto effect = createEffect(info, error);
    ASSERT_TRUE(effect) << error;

    effect->prepare(48000, 2);
    effect->reset();
    std::vector<float> buf(4, 1.0f);
    effect->process(buf.data(), 2);
    for (float v : buf) EXPECT_NEAR(v, 0.5f, 1e-5f);
}

#if !defined(_WIN32)
TEST(Vst3Effect, DoesNotImplementTheWindowsOnlyEditorInterfaceHere) {
    const auto info = fixtureInfo();
    ASSERT_FALSE(info.path.empty());
    std::string error;
    auto effect = createEffect(info, error);
    ASSERT_TRUE(effect) << error;
    EXPECT_EQ(dynamic_cast<hb::vst3::IVst3Editor*>(effect.get()), nullptr);
}
#endif
