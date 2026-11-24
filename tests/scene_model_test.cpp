#include <gtest/gtest.h>

#include "scene/scene.hpp"

using namespace hb;
using namespace hb::scene;

namespace {
bool parseScene(const std::string& text, Scene& s, std::string& err) {
    core::json::Value v;
    EXPECT_TRUE(core::json::parse(text, v));
    return sceneFromJson(v, s, err);
}
}  // namespace

TEST(SceneModel, ParsesColors) {
    Color c;
    EXPECT_TRUE(parseColor("#fff", c));
    EXPECT_EQ(c, (Color{255, 255, 255, 255}));
    EXPECT_TRUE(parseColor("#102030", c));
    EXPECT_EQ(c, (Color{0x10, 0x20, 0x30, 255}));
    EXPECT_TRUE(parseColor("#10203080", c));
    EXPECT_EQ(c.a, 0x80);
    EXPECT_FALSE(parseColor("102030", c));
    EXPECT_FALSE(parseColor("#12345", c));
    EXPECT_FALSE(parseColor("#gg0000", c));
    EXPECT_EQ(colorToString(Color{1, 2, 3, 255}), "#010203");
}

TEST(SceneModel, ParsesFullSceneAndRoundTrips) {
    Scene s;
    std::string err;
    ASSERT_TRUE(parseScene(R"({"id":"a.b","name":"Minha","background":{"type":"gradient","from":"#000","to":"#fff","angle":45},
        "layers":[{"role":"Pregador","slot":3,"rect":[0.1,0.2,0.3,0.4],"fit":"crop","zoom":2,"crop":[0.2,0.8],
        "radius":0.1,"border":{"width":0.01,"color":"#ff0000"},"shadow":{"blur":0.02,"dx":0.01,"dy":0.01,"opacity":0.4},
        "opacity":0.5,"z":2}],"overlays":[{"type":"lowerthird","text":"{nome}"}]})", s, err))
        << err;
    ASSERT_EQ(s.layers.size(), 1u);
    const auto& l = s.layers[0];
    EXPECT_EQ(l.role, "Pregador");
    EXPECT_EQ(l.slot, 3);
    EXPECT_EQ(l.fit, FitMode::Crop);
    EXPECT_DOUBLE_EQ(l.zoom, 2);
    EXPECT_DOUBLE_EQ(l.cropY, 0.8);
    EXPECT_TRUE(l.shadow.enabled());
    EXPECT_EQ(s.overlays.size(), 1u);

    Scene back;
    ASSERT_TRUE(sceneFromJson(sceneToJson(s), back, err)) << err;
    EXPECT_EQ(core::json::dump(sceneToJson(back)), core::json::dump(sceneToJson(s)));
}

TEST(SceneModel, ClampsAndRejectsHostileInput) {
    Scene s;
    std::string err;
    ASSERT_TRUE(parseScene(R"({"layers":[{"rect":[-50,9,0,1e9],"slot":99,"zoom":99,"radius":7,"opacity":-3}]})", s, err));
    const auto& l = s.layers[0];
    EXPECT_EQ(l.slot, -1);
    EXPECT_LE(l.rect.x, 2.0);
    EXPECT_GE(l.rect.x, -1.0);
    EXPECT_GT(l.rect.w, 0.0);
    EXPECT_LE(l.rect.h, 3.0);
    EXPECT_LE(l.zoom, 4.0);
    EXPECT_LE(l.radius, 0.5);
    EXPECT_GE(l.opacity, 0.0);

    EXPECT_FALSE(parseScene(R"({"layers":[{"rect":[1,2,3]}]})", s, err));
    EXPECT_FALSE(parseScene(R"({"layers":[{"fit":"stretchy"}]})", s, err));
    EXPECT_FALSE(parseScene(R"({"background":{"type":"video"}})", s, err));
    EXPECT_FALSE(parseScene(R"({"overlays":[{"nope":1}]})", s, err));
    std::string many = R"({"layers":[)";
    for (int i = 0; i < 40; ++i) many += (i ? ",{}" : "{}");
    many += "]}";
    EXPECT_FALSE(parseScene(many, s, err));
    EXPECT_FALSE(sceneFromJson(core::json::Value{}, s, err));
}

TEST(SceneModel, SanitizeAndRoles) {
    EXPECT_EQ(sanitizeText("  a|b\nc  ", 10), "abc");
    EXPECT_EQ(sanitizeId("a b/../c!"), "ab..c");
    // never splits a UTF-8 sequence
    const std::string t = sanitizeText("ççççç", 5);
    EXPECT_EQ(t.size() % 2, 0u);

    Scene s;
    std::string err;
    ASSERT_TRUE(parseScene(R"({"layers":[{"role":"A"},{"role":"B"},{"role":"A"},{}]})", s, err));
    EXPECT_EQ(rolesOf(s), (std::vector<std::string>{"A", "B"}));
    const Scene m = applyRoles(s, {{"A", 4}, {"B", 99}});
    EXPECT_EQ(m.layers[0].slot, 4);
    EXPECT_EQ(m.layers[1].slot, -1);
    EXPECT_EQ(m.layers[2].slot, 4);
    EXPECT_EQ(m.layers[3].slot, -1);
}
