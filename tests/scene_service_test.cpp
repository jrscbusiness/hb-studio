#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <map>

#include "app/engine.hpp"
#include "scene/scene_service.hpp"
#include "scene/template_library.hpp"

using namespace hb;
using namespace hb::scene;

namespace {
struct Fixture {
    std::map<int, std::shared_ptr<const Scene>> applied;
    SceneService make(const std::string& path = "") {
        return SceneService(path, [this](int slot, std::shared_ptr<const Scene> s) {
            if (s) applied[slot] = s; else applied.erase(slot);
        });
    }
};
std::string tmpPath(const char* n) { return std::string("/tmp/hb_scene_test_") + n; }
}  // namespace

TEST(SceneService, ApplyTemplateMapsRolesAndFillsFirstFreeSlots) {
    Fixture fx;
    auto svc = fx.make();
    const auto r = svc.handle(R"(sceneapply|culto|{"Pregador":2,"Slides":5,"Plano geral":0})");
    ASSERT_TRUE(r.ok) << r.message;
    EXPECT_TRUE(r.changed);
    EXPECT_EQ(fx.applied.size(), 5u);
    ASSERT_TRUE(fx.applied.count(0));
    EXPECT_EQ(fx.applied[0]->layers[0].slot, 2);          // "Pregador em tela cheia"
    EXPECT_EQ(fx.applied[2]->layers[1].slot, 2);          // slides + PiP: pregador PiP layer
    EXPECT_EQ(fx.applied[2]->layers[0].slot, 5);
    EXPECT_EQ(fx.applied[4]->layers[0].slot, 0);          // plano geral
    // applying again creates unique ids
    ASSERT_TRUE(svc.handle(R"(sceneapply|culto|{"Pregador":1}|pregador,letras)").ok);
    EXPECT_EQ(fx.applied.size(), 7u);
    EXPECT_NE(fx.applied[5]->id, fx.applied[0]->id);
    EXPECT_EQ(fx.applied[5]->layers[0].slot, 1);
}

TEST(SceneService, RefusesWhenThereIsNoRoomOrBadInput) {
    Fixture fx;
    auto svc = fx.make();
    for (int i = 0; i < 3; ++i) ASSERT_TRUE(svc.handle(R"(sceneapply|culto|{})").ok);
    // 3*5 = 15 used; one more apply of 5 does not fit
    const auto before = fx.applied.size();
    const auto r = svc.handle(R"(sceneapply|culto|{})");
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(fx.applied.size(), before);   // atomic: nothing partially applied
    EXPECT_FALSE(svc.handle(R"(sceneapply|nao-existe|{})").ok);
    EXPECT_FALSE(svc.handle("sceneapply|culto|not json").ok);
    EXPECT_FALSE(svc.handle("sceneapply|culto").ok);
    EXPECT_FALSE(svc.handle("scenedelete|nada").ok);
    EXPECT_FALSE(svc.handle("scenerename|nada|x").ok);
    EXPECT_FALSE(svc.handle("scenesave|{{{").ok);
    EXPECT_FALSE(svc.handle("scenebogus").ok);
}

TEST(SceneService, SaveRenameDeleteAndTemplateFromScene) {
    Fixture fx;
    auto svc = fx.make();
    ASSERT_TRUE(svc.handle(R"(scenesave|{"name":"Minha cena","layers":[{"slot":4,"rect":[0,0,0.5,0.5]}]})").ok);
    ASSERT_EQ(fx.applied.size(), 1u);
    const std::string id = fx.applied[0]->id;
    EXPECT_FALSE(id.empty());
    // upsert by id keeps the slot
    ASSERT_TRUE(svc.handle(R"(scenesave|{"id":")" + id + R"(","name":"Editada","layers":[]})").ok);
    EXPECT_EQ(fx.applied.size(), 1u);
    EXPECT_EQ(fx.applied[0]->name, "Editada");
    ASSERT_TRUE(svc.handle("scenerename|" + id + "|Outro nome").ok);
    EXPECT_EQ(fx.applied[0]->name, "Outro nome");

    ASSERT_TRUE(svc.handle(R"(scenesave|{"id":")" + id + R"(","name":"X","layers":[{"slot":4},{"role":"Host","slot":1}]})").ok);
    const auto t = svc.handle("scenetemplate|" + id + "|Meu layout");
    ASSERT_TRUE(t.ok) << t.message;
    ASSERT_EQ(svc.userTemplates().size(), 1u);
    const auto ut = svc.userTemplates()[0];
    EXPECT_EQ(ut.category, "meus");
    EXPECT_EQ(ut.roles, (std::vector<std::string>{"Câmera 5", "Host"}));
    EXPECT_EQ(ut.scenes[0].layers[0].slot, -1);
    EXPECT_EQ(ut.id, "u-Meulayout");
    EXPECT_NE(svc.stateJson().find("\"u-Meulayout\""), std::string::npos);

    ASSERT_TRUE(svc.handle("sceneapply|" + ut.id + R"(|{"Host":3,"Câmera 5":6})").ok);
    EXPECT_EQ(fx.applied.size(), 2u);
    ASSERT_TRUE(svc.handle("templatedelete|" + ut.id).ok);
    EXPECT_FALSE(svc.handle("templatedelete|culto").ok);   // built-ins are not deletable
    ASSERT_TRUE(svc.handle("scenedelete|" + id).ok);
    EXPECT_FALSE(fx.applied.count(0));
}

TEST(SceneService, PersistsAndRestoresScenesAndUserTemplates) {
    const std::string path = tmpPath("persist.json");
    std::remove(path.c_str());
    {
        Fixture fx;
        auto svc = fx.make(path);
        ASSERT_TRUE(svc.handle(R"(sceneapply|aula|{"Professor":0,"Tela compartilhada":1})").ok);
        ASSERT_TRUE(svc.handle("scenetemplate|" + fx.applied[0]->id + "|Salvo").ok);
    }
    Fixture fx2;
    auto svc2 = fx2.make(path);
    ASSERT_TRUE(svc2.load());
    EXPECT_EQ(fx2.applied.size(), 3u);
    EXPECT_EQ(fx2.applied[0]->layers[0].slot, 1);  // "Tela compartilhada" mapped to slot 1
    EXPECT_EQ(svc2.userTemplates().size(), 1u);
    std::remove(path.c_str());

    Fixture fx3;
    auto svc3 = fx3.make(tmpPath("missing.json"));
    EXPECT_FALSE(svc3.load());
    // hostile file
    { std::ofstream f(path); f << "{\"slots\":[{\"slot\":99,\"scene\":{}},{\"slot\":\"x\"},5],\"userTemplates\":[1,{}]}"; }
    Fixture fx4;
    auto svc4 = fx4.make(path);
    EXPECT_TRUE(svc4.load());
    EXPECT_TRUE(fx4.applied.empty());
    std::remove(path.c_str());
}

TEST(SceneService, ExportImportUserTemplateFile) {
    const std::string path = tmpPath("export.json");
    Fixture fx;
    auto svc = fx.make();
    ASSERT_TRUE(svc.handle("sceneexport|podcast-2|" + path).ok);
    // importing a built-in id never overwrites the built-in: it becomes a user template
    const auto r = svc.handle("sceneimport|" + path);
    ASSERT_TRUE(r.ok) << r.message;
    ASSERT_EQ(svc.userTemplates().size(), 1u);
    EXPECT_EQ(svc.userTemplates()[0].id, "u-podcast-2");
    EXPECT_FALSE(svc.userTemplates()[0].builtin);
    EXPECT_FALSE(svc.handle("sceneimport|/nao/existe.json").ok);
    { std::ofstream f(path); f << "garbage"; }
    EXPECT_FALSE(svc.handle("sceneimport|" + path).ok);
    std::remove(path.c_str());
}

TEST(SceneService, StateJsonIsValidAndListsTemplatesAndScenes) {
    Fixture fx;
    auto svc = fx.make();
    ASSERT_TRUE(svc.handle(R"(sceneapply|evento-telas|{})").ok);   // no roles needed
    core::json::Value v;
    ASSERT_TRUE(core::json::parse(svc.stateJson(), v));
    EXPECT_EQ(v.get("t")->s, "scenes_state");
    EXPECT_EQ(v.get("scenes")->a.size(), 3u);
    EXPECT_GE(v.get("templates")->a.size(), 9u);
    EXPECT_TRUE(v.get("templates")->a[0].get("builtin")->b);
}

TEST(SceneService, EngineExposesScenesOnTheSwitchBus) {
    app::Engine e({320, 180, {30, 1}});
    EXPECT_EQ(e.switcher().sourceCount(), app::Engine::kSourceCount + app::Engine::kSceneCount);
    video::Frame f;
    EXPECT_FALSE(e.renderUiFrame("c0", f));   // empty scene slot: nothing to draw
    EXPECT_FALSE(e.renderUiFrame("c16", f));
    EXPECT_FALSE(e.renderUiFrame("cx", f));
    SceneService svc("", [&](int slot, std::shared_ptr<const Scene> s) { e.setScene(slot, s); });
    ASSERT_TRUE(svc.handle(R"(sceneapply|culto|{"Pregador":0,"Slides":1,"Plano geral":2})").ok);
    EXPECT_TRUE(e.renderUiFrame("c0", f));
    EXPECT_TRUE(e.renderUiFrame("c4", f));
    EXPECT_FALSE(e.renderUiFrame("c5", f));
    // put scene 0 on the bus: PREVIEW then CUT
    const int idx = app::Engine::kSourceCount;
    ASSERT_TRUE(e.switcher().setPreview(idx));
    ASSERT_TRUE(e.switcher().cut());
    video::Frame pgm;
    video::prepareFrame(pgm, 320, 180);
    e.switcher().renderProgram(pgm, 0);
    video::Frame cam;
    video::prepareFrame(cam, 320, 180);
    e.switcher().source(0).render(cam, 0);
    EXPECT_EQ(pgm.data, cam.data);   // "Pregador em tela cheia" fills the frame with CAM 1
    EXPECT_FALSE(e.setScene(app::Engine::kSceneCount, nullptr));
}
