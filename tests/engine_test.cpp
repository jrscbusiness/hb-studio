#include <gtest/gtest.h>

#include <thread>

#include "app/engine.hpp"

using namespace hb;
using namespace std::chrono_literals;

TEST(Engine, ProducesProgramAndUiFrames) {
    app::Engine e({320, 180, {60, 1}});
    e.start();
    std::this_thread::sleep_for(300ms);
    auto s = e.snapshot();
    EXPECT_EQ(s.sw.pgm, 0);
    EXPECT_EQ(s.sourcesMax >= s.sourcesRecommended, true);
    EXPECT_GT(s.produced, 5u);

    const auto& defaultPreset = app::Engine::previewQualityPresets()[app::Engine::kDefaultPreviewQualityIndex];
    video::Frame f;
    EXPECT_TRUE(e.renderUiFrame("pgm", f));
    EXPECT_EQ(f.width, defaultPreset.monitorW);
    EXPECT_TRUE(e.renderUiFrame("pvw", f));
    EXPECT_TRUE(e.renderUiFrame("s7", f));
    EXPECT_EQ(f.width, defaultPreset.tileW);
    EXPECT_TRUE(e.renderUiFrame("s15", f));   // 2 banks of 8 (kSourceCount == 16): two-digit id works
    EXPECT_FALSE(e.renderUiFrame("s16", f));
    EXPECT_FALSE(e.renderUiFrame("s-1", f));
    EXPECT_FALSE(e.renderUiFrame("s", f));
    EXPECT_FALSE(e.renderUiFrame("nope", f));
    e.stop();
}

TEST(Engine, PreviewQualityIsSelectableAndAppliesImmediately) {
    app::Engine e({320, 180, {60, 1}});
    e.start();
    std::this_thread::sleep_for(150ms);
    EXPECT_EQ(e.previewQuality(), std::string("alta"));   // default preserves pre-existing behaviour

    EXPECT_FALSE(e.setPreviewQuality("nao-existe"));
    EXPECT_EQ(e.previewQuality(), std::string("alta"));   // rejected: unchanged

    ASSERT_TRUE(e.setPreviewQuality("baixa"));
    EXPECT_EQ(e.previewQuality(), std::string("baixa"));
    video::Frame f;
    EXPECT_TRUE(e.renderUiFrame("pgm", f));
    const auto& baixa = app::Engine::previewQualityPresets()[0];
    EXPECT_EQ(baixa.id, std::string("baixa"));
    EXPECT_EQ(f.width, baixa.monitorW);
    EXPECT_TRUE(e.renderUiFrame("s0", f));
    EXPECT_EQ(f.width, baixa.tileW);

    ASSERT_TRUE(e.setPreviewQuality("maxima"));
    const auto& maxima = app::Engine::previewQualityPresets().back();
    EXPECT_EQ(maxima.id, std::string("maxima"));
    EXPECT_TRUE(e.renderUiFrame("pgm", f));
    EXPECT_EQ(f.width, maxima.monitorW);
    e.stop();
}

TEST(Engine, SwitcherChangesReachProgramAndLog) {
    app::Engine e({320, 180, {60, 1}});
    e.start();
    e.switcher().setPreview(4);
    e.switcher().cut();
    std::this_thread::sleep_for(150ms);
    EXPECT_EQ(e.snapshot().sw.pgm, 4);
    bool logged = false;
    for (auto& r : e.logTail()) logged |= r.message.find("programChanged") != std::string::npos;
    EXPECT_TRUE(logged);
    e.stop();
}

TEST(Engine, DissolveCompletesWhileRunning) {
    app::Engine e({320, 180, {60, 1}});
    e.start();
    e.switcher().startDissolve(200);
    std::this_thread::sleep_for(500ms);
    auto s = e.snapshot();
    EXPECT_FALSE(s.sw.transitioning);
    EXPECT_EQ(s.sw.pgm, 1);
    e.stop();
}
