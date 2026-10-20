#include <gtest/gtest.h>

#include "switcher/switcher.hpp"
#include "video/frame_ops.hpp"

using namespace hb;
using Clock = switcher::Switcher::Clock;
using namespace std::chrono;

namespace {
std::vector<std::shared_ptr<const video::Source>> make(int n = 4) {
    std::vector<std::shared_ptr<const video::Source>> v;
    for (int i = 0; i < n; ++i) v.push_back(std::make_shared<video::SyntheticSource>(i + 1, "CAM"));
    return v;
}
video::Frame frame(int w = 64, int h = 36) {
    video::Frame f;
    video::prepareFrame(f, w, h);
    return f;
}
}  // namespace

TEST(Synthetic, SourcesDifferAndAreDeterministic) {
    video::SyntheticSource a(1, "a"), b(2, "b");
    auto f1 = frame(), f2 = frame(), f3 = frame();
    a.render(f1, 0); a.render(f2, 0); b.render(f3, 0);
    EXPECT_EQ(f1.data, f2.data);
    EXPECT_NE(f1.data, f3.data);
    auto f4 = frame();
    a.render(f4, 45);
    EXPECT_NE(f1.data, f4.data);  // animates
}

TEST(Synthetic, WorksAtAnySizeAndIsOpaque) {
    video::SyntheticSource s(7, "s");
    for (auto [w, h] : {std::pair{1, 1}, {16, 9}, {192, 108}, {1920, 1080}}) {
        auto f = frame(w, h);
        EXPECT_NO_THROW(s.render(f, 3));
        EXPECT_EQ(f.data[3], 255);
    }
}

TEST(Switcher, InitialStateAndPreview) {
    switcher::Switcher sw(make());
    auto s = sw.state();
    EXPECT_EQ(s.pgm, 0);
    EXPECT_EQ(s.pvw, 1);
    EXPECT_TRUE(sw.setPreview(3));
    EXPECT_EQ(sw.state().pvw, 3);
    EXPECT_FALSE(sw.setPreview(4));
    EXPECT_FALSE(sw.setPreview(-1));
}

TEST(Switcher, CutSwapsPreviewAndProgram) {
    core::EventBus bus;
    std::vector<std::string> topics;
    bus.subscribe("*", [&](const core::Event& e) { topics.push_back(e.topic); });
    switcher::Switcher sw(make(), &bus);
    sw.setPreview(2);
    EXPECT_TRUE(sw.cut());
    EXPECT_EQ(sw.state().pgm, 2);
    EXPECT_EQ(sw.state().pvw, 0);
    EXPECT_EQ(topics, (std::vector<std::string>{"switcher.previewChanged", "switcher.programChanged"}));
}

TEST(Switcher, CutWithSameSourceIsNoop) {
    switcher::Switcher sw(make());
    sw.setPreview(0);
    EXPECT_FALSE(sw.cut());
}

TEST(Switcher, ProgramBusDoesNotTouchPreview) {
    switcher::Switcher sw(make());
    sw.setPreview(3);
    EXPECT_TRUE(sw.setProgram(2));
    EXPECT_EQ(sw.state().pgm, 2);
    EXPECT_EQ(sw.state().pvw, 3);
}

TEST(Switcher, ProgramRendersSelectedSource) {
    auto srcs = make();
    switcher::Switcher sw(srcs);
    auto out = frame(), ref = frame();
    sw.renderProgram(out, 5);
    srcs[0]->render(ref, 5);
    EXPECT_EQ(out.data, ref.data);
    sw.setPreview(2);
    sw.cut();
    sw.renderProgram(out, 5);
    srcs[2]->render(ref, 5);
    EXPECT_EQ(out.data, ref.data);
}

TEST(Switcher, DissolveBlendsThenCompletes) {
    core::EventBus bus;
    std::vector<std::string> topics;
    bus.subscribe("transition.*", [&](const core::Event& e) { topics.push_back(e.topic); });
    auto srcs = make();
    switcher::Switcher sw(srcs, &bus);
    const auto t0 = Clock::now();
    ASSERT_TRUE(sw.startDissolve(1000, t0));
    EXPECT_TRUE(sw.state(t0).transitioning);
    EXPECT_FALSE(sw.cut());            // locked during transition
    EXPECT_FALSE(sw.setPreview(2));

    auto a = frame(), b = frame(), mid = frame();
    srcs[0]->render(a, 7);
    srcs[1]->render(b, 7);
    sw.renderProgram(mid, 7, t0 + milliseconds(500));
    bool strictlyBetween = false;
    for (size_t i = 0; i < mid.data.size(); ++i) {
        const int lo = std::min(a.data[i], b.data[i]), hi = std::max(a.data[i], b.data[i]);
        EXPECT_GE(mid.data[i], lo - 1);
        EXPECT_LE(mid.data[i], hi + 1);
        if (a.data[i] != b.data[i] && mid.data[i] > lo + 1 && mid.data[i] < hi - 1) strictlyBetween = true;
    }
    EXPECT_TRUE(strictlyBetween);
    EXPECT_NEAR(sw.state(t0 + milliseconds(500)).progress, 0.5f, 0.01f);

    auto end = frame();
    sw.renderProgram(end, 7, t0 + milliseconds(1000));
    EXPECT_EQ(end.data, b.data);       // fully the new source
    auto s = sw.state(t0 + seconds(2));
    EXPECT_FALSE(s.transitioning);
    EXPECT_EQ(s.pgm, 1);
    EXPECT_EQ(s.pvw, 0);
    EXPECT_EQ(topics, (std::vector<std::string>{"transition.started", "transition.finished"}));
}

TEST(Switcher, ZeroDurationDissolveIsACut) {
    switcher::Switcher sw(make());
    EXPECT_TRUE(sw.startDissolve(0));
    EXPECT_EQ(sw.state().pgm, 1);
}

TEST(FrameOps, DownscaleBoxAveragesAndMixClamps) {
    video::Frame src;
    video::prepareFrame(src, 4, 4);
    for (size_t i = 0; i < src.data.size(); i += 4) {
        const bool bright = (i / 4) % 2;
        src.data[i] = src.data[i + 1] = src.data[i + 2] = bright ? 200 : 100;
        src.data[i + 3] = 255;
    }
    video::Frame dst;
    video::downscaleBox(src, dst, 2, 2);
    EXPECT_EQ(dst.width, 2);
    EXPECT_EQ(dst.data[0], 150);
    video::Frame a = src, b = src, o;
    video::mixFrames(a, b, o, 9999);  // clamped
    EXPECT_EQ(o.data, src.data);
}
