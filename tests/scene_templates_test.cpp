#include <gtest/gtest.h>

#include <set>

#include "scene/compositor.hpp"
#include "scene/template_library.hpp"
#include "video/frame_ops.hpp"

using namespace hb::scene;

TEST(SceneTemplates, BuiltinLibraryParsesWithNoErrors) {
    std::vector<Template> v;
    std::vector<std::string> errs;
    ASSERT_TRUE(parseTemplateDocument(builtinTemplatesJson(), v, errs, true));
    EXPECT_TRUE(errs.empty()) << (errs.empty() ? "" : errs.front());
    EXPECT_EQ(v.size(), builtinTemplates().size());
    EXPECT_GE(v.size(), 9u);
}

TEST(SceneTemplates, CoversTheRequestedCategories) {
    std::set<std::string> cats, ids;
    for (const auto& t : builtinTemplates()) {
        cats.insert(t.category);
        EXPECT_TRUE(ids.insert(t.id).second) << "duplicate id " << t.id;
        EXPECT_TRUE(t.builtin);
        EXPECT_FALSE(t.scenes.empty());
    }
    for (const char* c : {"culto", "aula", "podcast", "evento"}) EXPECT_TRUE(cats.count(c)) << c;
    int scenes = 0;
    for (const auto& t : builtinTemplates()) scenes += static_cast<int>(t.scenes.size());
    EXPECT_GE(scenes, 25);
}

TEST(SceneTemplates, EveryRoleLayerHasADeclaredRoleAndEverySceneRenders) {
    for (const auto& t : builtinTemplates()) {
        for (const auto& s : t.scenes) {
            for (const auto& l : s.layers) {
                EXPECT_FALSE(l.role.empty()) << t.id << "/" << s.id;
                EXPECT_EQ(l.slot, -1);
                bool declared = false;
                for (const auto& r : t.roles) declared = declared || r == l.role;
                EXPECT_TRUE(declared) << t.id << " role " << l.role;
            }
            hb::video::Frame f;
            renderThumbnail(s, 160, 90, f);
            ASSERT_EQ(f.data.size(), 160u * 90u * 4u);
            EXPECT_EQ(f.data[3], 255);
        }
    }
}

TEST(SceneTemplates, ThumbnailShowsPlaceholderPerRole) {
    for (const auto& t : builtinTemplates()) {
        if (t.id != "culto") continue;
        hb::video::Frame f;
        renderThumbnail(t.scenes[0], 160, 90, f);  // Pregador full screen
        const Color pc = placeholderColor("Pregador", -1);
        EXPECT_EQ(f.data[(45 * 160 + 80) * 4 + 2], pc.r);
        return;
    }
    FAIL() << "culto template missing";
}

TEST(SceneTemplates, DocumentRoundTripAndValidation) {
    const std::string doc = templateDocument(builtinTemplates());
    std::vector<Template> v;
    std::vector<std::string> errs;
    ASSERT_TRUE(parseTemplateDocument(doc, v, errs));
    EXPECT_TRUE(errs.empty());
    ASSERT_EQ(v.size(), builtinTemplates().size());
    EXPECT_FALSE(v[0].builtin);
    EXPECT_EQ(templateDocument(v), doc);
}

TEST(SceneTemplates, RejectsBadDocuments) {
    std::vector<Template> v;
    std::vector<std::string> errs;
    EXPECT_FALSE(parseTemplateDocument("not json", v, errs));
    EXPECT_FALSE(parseTemplateDocument(R"({"format":"other","version":1,"templates":[]})", v, errs));
    EXPECT_FALSE(parseTemplateDocument(R"({"format":"hb-templates","version":99,"templates":[]})", v, errs));
    EXPECT_FALSE(parseTemplateDocument(R"({"format":"hb-templates","templates":[]})", v, errs));
    EXPECT_TRUE(v.empty());
    errs.clear();
    // a bad template does not stop a good one
    ASSERT_TRUE(parseTemplateDocument(R"({"format":"hb-templates","version":1,"templates":[
        {"id":"","scenes":[]},{"id":"ok","name":"Ok","scenes":[{"id":"a","layers":[{"role":"X"}]}]}]})", v, errs));
    ASSERT_EQ(v.size(), 1u);
    EXPECT_EQ(v[0].roles, std::vector<std::string>{"X"});
    EXPECT_EQ(errs.size(), 1u);
}
