#include <gtest/gtest.h>

#include "app/template_overlays.hpp"

using namespace hb;

namespace {
core::json::Value parse(const char* text) {
    core::json::Value v;
    EXPECT_TRUE(core::json::parse(text, v));
    return v;
}
}  // namespace

TEST(TemplateOverlays, CreatesOffAirElementsFromPresetsWithoutBraces) {
    overlay::OverlayManager mgr;
    scene::Scene s;
    s.id = "apresentacao-lt";
    s.name = "Apresentação";
    s.overlays.push_back(parse(R"({"type":"lowerthird","text":"{Palestrante}","subtitle":"{cargo}"})"));
    EXPECT_EQ(app::createOverlaysFromScene(mgr, s), 1);
    const auto el = mgr.element("tpl-apresentacao-lt-1");
    ASSERT_TRUE(el);
    EXPECT_EQ(el->config().title, "Palestrante");
    EXPECT_EQ(el->config().subtitle, "cargo");
    EXPECT_EQ(el->config().type, "lowerthird");
}

TEST(TemplateOverlays, ReapplyingNeverClobbersTheOperatorsEdits) {
    overlay::OverlayManager mgr;
    scene::Scene s;
    s.id = "x";
    s.name = "X";
    s.overlays.push_back(parse(R"({"type":"lowerthird","text":"{Nome}"})"));
    ASSERT_EQ(app::createOverlaysFromScene(mgr, s), 1);
    auto cfg = mgr.element("tpl-x-1")->config();
    cfg.title = "Maria Souza";
    ASSERT_FALSE(mgr.upsert(cfg).empty());
    EXPECT_EQ(app::createOverlaysFromScene(mgr, s), 0);   // already exists: left alone
    EXPECT_EQ(mgr.element("tpl-x-1")->config().title, "Maria Souza");
}

TEST(TemplateOverlays, HostilePresetsAreIgnored) {
    overlay::OverlayManager mgr;
    scene::Scene s;
    s.id = "h";
    s.name = "H";
    s.overlays.push_back(parse("5"));
    s.overlays.push_back(parse(R"({"type":"not-a-type"})"));
    s.overlays.push_back(parse(R"({"type":5})"));
    s.overlays.push_back(parse("[]"));
    EXPECT_EQ(app::createOverlaysFromScene(mgr, s), 0);
    EXPECT_TRUE(mgr.ids().empty());
}
