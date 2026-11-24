#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#include "scene/compositor.hpp"
#include "scene/scene_source.hpp"
#include "switcher/switcher.hpp"
#include "video/frame_ops.hpp"

using namespace hb;
using namespace hb::scene;

namespace {

class Flat final : public video::Source {
public:
    Flat(std::uint8_t b, std::uint8_t g, std::uint8_t r, std::uint8_t a = 255) : b_(b), g_(g), r_(r), a_(a) {}
    const std::string& name() const override { return name_; }
    bool isTestSource() const override { return true; }
    void render(video::Frame& f, std::uint64_t) const override {
        for (std::size_t i = 0; i + 3 < f.data.size(); i += 4) { f.data[i] = b_; f.data[i + 1] = g_; f.data[i + 2] = r_; f.data[i + 3] = a_; }
    }
private:
    std::string name_ = "flat";
    std::uint8_t b_, g_, r_, a_;
};

Layer layer(int slot, double x, double y, double w, double h, FitMode fit = FitMode::Fill) {
    Layer l;
    l.slot = slot;
    l.rect = {x, y, w, h};
    l.fit = fit;
    return l;
}

const std::uint8_t* px(const video::Frame& f, int x, int y) { return f.data.data() + static_cast<std::size_t>(y) * f.stride + x * 4; }

std::vector<std::shared_ptr<const video::Source>> inputs() {
    return {std::make_shared<Flat>(0, 0, 255),      // slot 0: red (BGR order)
            std::make_shared<Flat>(0, 255, 0),      // slot 1: green
            std::make_shared<Flat>(255, 0, 0)};     // slot 2: blue
}

}  // namespace

TEST(SceneCompositor, BackgroundSolidAndGradient) {
    Scene s;
    s.bg.from = {10, 20, 30, 255};
    video::Frame f;
    renderThumbnail(s, 32, 18, f);
    EXPECT_EQ(px(f, 5, 5)[0], 30);
    EXPECT_EQ(px(f, 5, 5)[2], 10);

    s.bg.type = Background::Type::Gradient;
    s.bg.from = {0, 0, 0, 255};
    s.bg.to = {255, 255, 255, 255};
    s.bg.angle = 90;  // top -> bottom
    renderThumbnail(s, 32, 64, f);
    EXPECT_LT(px(f, 16, 2)[0], 20);
    EXPECT_GT(px(f, 16, 61)[0], 235);
    EXPECT_NEAR(px(f, 16, 32)[0], 128, 12);
}

TEST(SceneCompositor, LayersAreScaledIntoTheirRectAndOrderedByZ) {
    SceneSource src("cena", inputs());
    auto sc = std::make_shared<Scene>();
    sc->bg.from = {0, 0, 0, 255};
    sc->layers = {layer(0, 0, 0, 0.5, 1), layer(1, 0.5, 0, 0.5, 1)};
    src.setScene(sc);
    video::Frame f;
    video::prepareFrame(f, 160, 90);
    src.render(f, 0);
    EXPECT_EQ(px(f, 20, 45)[2], 255);   // left half: red
    EXPECT_EQ(px(f, 20, 45)[1], 0);
    EXPECT_EQ(px(f, 140, 45)[1], 255);  // right half: green

    // z-order: a higher-z layer drawn later covers the others regardless of array order
    auto sc2 = std::make_shared<Scene>(*sc);
    sc2->layers = {layer(2, 0.25, 0.25, 0.5, 0.5), layer(0, 0, 0, 1, 1)};
    sc2->layers[0].z = 5;
    src.setScene(sc2);
    src.render(f, 0);
    EXPECT_EQ(px(f, 80, 45)[0], 255);  // blue on top in the middle
    EXPECT_EQ(px(f, 5, 5)[2], 255);    // red below at the corner
}

TEST(SceneCompositor, FitLetterboxesAndFillCoversWithPlaceholderAspect) {
    // 4:3 box inside a 16:9 frame at "fit": the 16:9 source is letterboxed, background visible.
    SceneSource src("c", inputs());
    auto sc = std::make_shared<Scene>();
    sc->bg.from = {0, 0, 255, 255};  // blue background
    sc->layers = {layer(0, 0.25, 0.1, 0.5, 0.8, FitMode::Fit)};
    src.setScene(sc);
    video::Frame f;
    video::prepareFrame(f, 320, 180);
    src.render(f, 0);
    // box = 160x144 px at (80,18); 16:9 content = 160x90, centred vertically => rows 45..135
    EXPECT_EQ(px(f, 160, 90)[2], 255);   // red content
    EXPECT_EQ(px(f, 160, 30)[0], 255);   // above content (inside box): blue background
    EXPECT_EQ(px(f, 160, 30)[2], 0);

    sc = std::make_shared<Scene>(*sc);
    sc->layers[0].fit = FitMode::Fill;
    src.setScene(sc);
    src.render(f, 0);
    EXPECT_EQ(px(f, 160, 30)[2], 255);   // now the box is fully covered
}

TEST(SceneCompositor, RoundedCornersAndBorder) {
    SceneSource src("c", inputs());
    auto sc = std::make_shared<Scene>();
    sc->bg.from = {0, 0, 0, 255};
    Layer l = layer(1, 0.1, 0.1, 0.8, 0.8);
    l.radius = 0.5;  // circle-ish
    l.border.width = 0.03;
    l.border.color = {255, 255, 255, 255};
    sc->layers = {l};
    src.setScene(sc);
    video::Frame f;
    video::prepareFrame(f, 200, 200);
    src.render(f, 0);
    EXPECT_EQ(px(f, 100, 100)[1], 255);            // centre: green content
    EXPECT_EQ(px(f, 100, 100)[2], 0);
    EXPECT_EQ(px(f, 22, 22)[1], 0);                // corner of the box is cut away: background
    EXPECT_EQ(px(f, 22, 22)[0], 0);
    EXPECT_GT(px(f, 100, 22)[0], 200);             // top edge: white border ring
    EXPECT_GT(px(f, 100, 22)[2], 200);
}

TEST(SceneCompositor, DropShadowDarkensOutsideTheLayerOnly) {
    SceneSource src("c", inputs());
    auto sc = std::make_shared<Scene>();
    sc->bg.from = {200, 200, 200, 255};
    Layer l = layer(0, 0.3, 0.3, 0.4, 0.4);
    l.shadow = {0.06, 0.0, 0.03, 0.8};
    sc->layers = {l};
    src.setScene(sc);
    video::Frame f;
    video::prepareFrame(f, 200, 200);
    src.render(f, 0);
    EXPECT_EQ(px(f, 5, 5)[0], 200);               // far away: untouched
    EXPECT_LT(px(f, 100, 126)[0], 190);           // just below the layer: shadowed
    EXPECT_EQ(px(f, 100, 100)[2], 255);           // content unaffected
}

TEST(SceneCompositor, OpacityBlends) {
    SceneSource src("c", inputs());
    auto sc = std::make_shared<Scene>();
    sc->bg.from = {0, 0, 0, 255};
    Layer l = layer(0, 0, 0, 1, 1);
    l.z = 1;
    Layer under = layer(1, 0, 0, 1, 1);
    l.opacity = 0.5;
    sc->layers = {under, l};
    src.setScene(sc);
    video::Frame f;
    video::prepareFrame(f, 32, 18);
    src.render(f, 0);
    EXPECT_NEAR(px(f, 10, 10)[2], 128, 3);
    EXPECT_NEAR(px(f, 10, 10)[1], 127, 3);
}

TEST(SceneCompositor, SourceTransparencyShowsLayersBelowAndBackground) {
    std::vector<std::shared_ptr<const video::Source>> in{std::make_shared<Flat>(0, 0, 255, 0),  // fully transparent "keyed" cam
                                                         std::make_shared<Flat>(0, 255, 0)};
    SceneSource src("c", in);
    auto sc = std::make_shared<Scene>();
    sc->bg.from = {0, 0, 255, 255};
    // bottom layer is full-frame (direct path) and transparent -> background must show through
    sc->layers = {layer(0, 0, 0, 1, 1)};
    src.setScene(sc);
    video::Frame f;
    video::prepareFrame(f, 32, 18);
    src.render(f, 0);
    EXPECT_EQ(px(f, 4, 4)[0], 255);
    EXPECT_EQ(px(f, 4, 4)[3], 255);
    // transparent layer on top of a green one shows green
    sc = std::make_shared<Scene>();
    sc->layers = {layer(1, 0, 0, 1, 1), layer(0, 0, 0, 1, 1)};
    sc->layers[1].z = 1;
    src.setScene(sc);
    src.render(f, 0);
    EXPECT_EQ(px(f, 4, 4)[1], 255);
}

TEST(SceneCompositor, UnassignedLayerDrawsPlaceholderAndNeverCrashes) {
    SceneSource src("c", inputs());
    auto sc = std::make_shared<Scene>();
    sc->layers = {layer(-1, 0, 0, 0.5, 1), layer(77, 0.5, 0, 0.5, 1)};
    sc->layers[0].role = "Pregador";
    src.setScene(sc);
    video::Frame f;
    video::prepareFrame(f, 64, 36);
    src.render(f, 0);
    const Color pc = placeholderColor("Pregador", -1);
    EXPECT_EQ(px(f, 10, 10)[2], pc.r);
    // degenerate sizes
    video::Frame tiny;
    video::prepareFrame(tiny, 1, 1);
    src.render(tiny, 0);
    video::Frame empty;
    src.render(empty, 0);
}

TEST(SceneCompositor, EmptySlotRendersDarkPanelAndIsATestSource) {
    SceneSource src("c", inputs());
    EXPECT_TRUE(src.isTestSource());
    video::Frame f;
    video::prepareFrame(f, 16, 9);
    src.render(f, 0);
    EXPECT_EQ(px(f, 1, 1)[3], 255);
    src.setScene(std::make_shared<Scene>());
    EXPECT_FALSE(src.isTestSource());
}

TEST(SceneCompositor, CompilesOncePerSizeAndPrepareWarmsTheCache) {
    SceneSource src("c", inputs());
    auto sc = std::make_shared<Scene>();
    sc->layers = {layer(0, 0.1, 0.1, 0.5, 0.5)};
    sc->layers[0].radius = 0.2;
    src.setScene(sc);
    src.prepare(160, 90);
    EXPECT_EQ(src.compileCount(), 1u);
    video::Frame f;
    video::prepareFrame(f, 160, 90);
    for (int i = 0; i < 5; ++i) src.render(f, i);
    EXPECT_EQ(src.compileCount(), 1u);
    video::Frame g;
    video::prepareFrame(g, 80, 45);
    src.render(g, 0);
    EXPECT_EQ(src.compileCount(), 2u);
}

TEST(SceneCompositor, AspectFromSourceChangesGeometry) {
    double aspect = 16.0 / 9.0;
    SceneSource src("c", inputs(), [&](int) { return aspect; });
    auto sc = std::make_shared<Scene>();
    sc->bg.from = {0, 0, 255, 255};
    sc->layers = {layer(0, 0, 0, 1, 1, FitMode::Fit)};
    src.setScene(sc);
    video::Frame f;
    video::prepareFrame(f, 160, 90);
    src.render(f, 0);
    EXPECT_EQ(px(f, 80, 45)[2], 255);
    aspect = 1.0;  // a square camera: pillarboxed
    src.render(f, 0);
    EXPECT_EQ(px(f, 5, 45)[0], 255);   // left bar: background
    EXPECT_EQ(px(f, 80, 45)[2], 255);
}

TEST(SceneCompositor, BmpBackgroundDecodes) {
    // 2x2 24-bit bottom-up BMP: rows (bottom) red,green / (top) blue,white
    std::vector<std::uint8_t> b = {'B', 'M', 0, 0, 0, 0, 0, 0, 0, 0, 54, 0, 0, 0, 40, 0, 0, 0, 2, 0, 0, 0, 2, 0, 0, 0, 1, 0, 24, 0,
                                   0, 0, 0, 0, 16, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                   0, 0, 255, 0, 255, 0, 0, 0,           // bottom row: red, green (+2 pad)
                                   255, 0, 0, 255, 255, 255, 0, 0};      // top row: blue, white
    video::Frame f;
    ASSERT_TRUE(decodeBmp(b.data(), b.size(), f));
    EXPECT_EQ(f.width, 2);
    EXPECT_EQ(px(f, 0, 0)[0], 255);  // top-left blue
    EXPECT_EQ(px(f, 0, 1)[2], 255);  // bottom-left red
    EXPECT_FALSE(decodeBmp(b.data(), 40, f));
    b[28] = 8;  // 8 bpp unsupported
    EXPECT_FALSE(decodeBmp(b.data(), b.size(), f));

    Scene s;
    s.bg.type = Background::Type::Image;
    s.bg.imagePath = "/definitely/missing.bmp";
    s.bg.from = {1, 2, 3, 255};
    renderThumbnail(s, 16, 9, f);  // falls back to the solid colour
    EXPECT_EQ(px(f, 3, 3)[0], 3);
}

TEST(SceneCompositor, SceneIsATransitionableSwitcherSource) {
    auto flat = std::make_shared<Flat>(0, 0, 255);
    auto scn = std::make_shared<SceneSource>("CENA 1", std::vector<std::shared_ptr<const video::Source>>{std::make_shared<Flat>(0, 255, 0)});
    auto sc = std::make_shared<Scene>();
    sc->layers = {layer(0, 0, 0, 1, 1)};
    scn->setScene(sc);
    switcher::Switcher sw({flat, scn});
    ASSERT_TRUE(sw.setPreview(1));
    ASSERT_TRUE(sw.cut());
    video::Frame f;
    video::prepareFrame(f, 32, 18);
    sw.renderProgram(f, 0);
    EXPECT_EQ(px(f, 4, 4)[1], 255);   // program is the scene (green)
    // dissolve back to the red camera
    ASSERT_TRUE(sw.setPreview(0));
    const auto t0 = switcher::Switcher::Clock::now();
    ASSERT_TRUE(sw.startDissolve(1000, t0));
    sw.renderProgram(f, 0, t0 + std::chrono::milliseconds(500));
    EXPECT_NEAR(px(f, 4, 4)[1], 128, 3);
    EXPECT_NEAR(px(f, 4, 4)[2], 128, 3);
}

TEST(SceneCompositor, ConcurrentRendersAreSafeAndFastEnough) {
    SceneSource src("c", inputs());
    auto sc = std::make_shared<Scene>();
    sc->bg.type = Background::Type::Gradient;
    for (int i = 0; i < 4; ++i) {
        Layer l = layer(i % 3, 0.05 + 0.2 * i, 0.1 + 0.1 * i, 0.4, 0.4);
        l.radius = 0.1;
        l.border.width = 0.004;
        l.shadow = {0.02, 0.004, 0.008, 0.5};
        sc->layers.push_back(l);
    }
    src.setScene(sc);
    src.prepare(640, 360);
    std::vector<std::thread> th;
    for (int t = 0; t < 3; ++t)
        th.emplace_back([&] {
            video::Frame f;
            video::prepareFrame(f, 640, 360);
            for (int i = 0; i < 30; ++i) src.render(f, i);
        });
    for (auto& t : th) t.join();
    EXPECT_EQ(src.compileCount(), 1u);

    video::Frame f;
    video::prepareFrame(f, 1920, 1080);
    src.prepare(1920, 1080);
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 5; ++i) src.render(f, i);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 5;
    RecordProperty("ms_per_1080p_frame", static_cast<int>(ms));
    EXPECT_LT(ms, 200.0);  // very loose: guards against accidental O(n^2) regressions only
}
