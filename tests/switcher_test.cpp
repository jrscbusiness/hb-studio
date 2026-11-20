#include <gtest/gtest.h>

#include <thread>

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

TEST(Switcher, TBarBlendsProportionallyToPosition) {
    core::EventBus bus;
    std::vector<std::string> topics;
    bus.subscribe("transition.*", [&](const core::Event& e) { topics.push_back(e.topic); });
    auto srcs = make();
    switcher::Switcher sw(srcs, &bus);
    ASSERT_TRUE(sw.setTBarPosition(0.5f));
    EXPECT_TRUE(sw.state().transitioning);
    EXPECT_NEAR(sw.state().progress, 0.5f, 1e-6f);
    EXPECT_FALSE(sw.cut());             // a timed op is still locked out during a manual transition
    EXPECT_FALSE(sw.startDissolve(500));

    auto a = frame(), b = frame(), mid = frame();
    srcs[0]->render(a, 3);
    srcs[1]->render(b, 3);
    sw.renderProgram(mid, 3);
    bool strictlyBetween = false;
    for (size_t i = 0; i < mid.data.size(); ++i)
        if (a.data[i] != b.data[i] && mid.data[i] != a.data[i] && mid.data[i] != b.data[i]) strictlyBetween = true;
    EXPECT_TRUE(strictlyBetween);

    // Dragging further updates the mix live, still without completing.
    ASSERT_TRUE(sw.setTBarPosition(0.9f));
    EXPECT_TRUE(sw.state().transitioning);
    EXPECT_NEAR(sw.state().progress, 0.9f, 1e-6f);
    EXPECT_EQ(sw.state().pvw, 1);
    EXPECT_EQ(sw.state().pgm, 0);
    EXPECT_EQ(topics, (std::vector<std::string>{"transition.started"}));
}

TEST(Switcher, TBarReachingOneCompletesLikeADissolve) {
    core::EventBus bus;
    std::vector<std::string> topics;
    bus.subscribe("transition.*", [&](const core::Event& e) { topics.push_back(e.topic); });
    switcher::Switcher sw(make(), &bus);
    ASSERT_TRUE(sw.setTBarPosition(0.5f));
    ASSERT_TRUE(sw.setTBarPosition(1.0f));
    auto s = sw.state();
    EXPECT_FALSE(s.transitioning);
    EXPECT_EQ(s.pgm, 1);
    EXPECT_EQ(s.pvw, 0);
    EXPECT_EQ(topics, (std::vector<std::string>{"transition.started", "transition.finished"}));
    // A fresh CUT/DISSOLVE works normally right after.
    EXPECT_TRUE(sw.cut());
}

TEST(Switcher, TBarBackToZeroCancelsWithoutSwapping) {
    switcher::Switcher sw(make());
    ASSERT_TRUE(sw.setTBarPosition(0.7f));
    ASSERT_TRUE(sw.setTBarPosition(0.f));
    auto s = sw.state();
    EXPECT_FALSE(s.transitioning);
    EXPECT_EQ(s.pgm, 0);   // unchanged: cancelled, not completed
    EXPECT_EQ(s.pvw, 1);
    EXPECT_TRUE(sw.cut());  // not locked out afterwards
}

TEST(Switcher, TBarOutOfRangeIsClampedAndNothingToCrossFadeIsRejected) {
    switcher::Switcher sw(make());
    EXPECT_TRUE(sw.setTBarPosition(-5.f));   // clamped to 0: starts, then immediately cancels
    EXPECT_FALSE(sw.state().transitioning);
    EXPECT_EQ(sw.state().pgm, 0);            // unchanged (cancelled, not completed)
    EXPECT_TRUE(sw.setTBarPosition(50.f));   // clamped to 1: starts, then immediately completes
    EXPECT_FALSE(sw.state().transitioning);
    EXPECT_EQ(sw.state().pgm, 1);

    switcher::Switcher single(std::vector<std::shared_ptr<const video::Source>>{
        std::make_shared<video::SyntheticSource>(1, "CAM")});
    EXPECT_FALSE(single.setTBarPosition(0.5f));  // pvw == pgm: nothing to cross-fade
}

TEST(Switcher, TimedDissolveLocksOutTheTBarAndViceVersa) {
    switcher::Switcher sw(make());
    const auto t0 = Clock::now();
    ASSERT_TRUE(sw.startDissolve(1000, t0));
    EXPECT_FALSE(sw.setTBarPosition(0.5f));   // a timed dissolve owns the mix
    auto mid = sw.state(t0 + milliseconds(500));
    EXPECT_NEAR(mid.progress, 0.5f, 0.01f);
    auto doneFrame = frame();
    sw.renderProgram(doneFrame, 1, t0 + seconds(2));  // let it finish

    ASSERT_TRUE(sw.setTBarPosition(0.3f));
    EXPECT_FALSE(sw.startDissolve(100));       // a manual transition owns the mix
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

// ---- Regressions from REVIEW-1 (BAIXO) ----
TEST(FrameOps, MixWithDifferentSizesIsSafe) {
    video::Frame a, b, o;
    video::prepareFrame(a, 8, 8);
    video::prepareFrame(b, 4, 4);
    EXPECT_NO_THROW(video::mixFrames(a, b, o, 128));
    EXPECT_EQ(o.data.size(), a.data.size());
}

TEST(FrameOps, DownscaleLargeBlocksDoNotOverflow) {
    video::Frame src;
    video::prepareFrame(src, 7680, 4320);
    std::fill(src.data.begin(), src.data.end(), std::uint8_t{255});
    video::Frame dst;
    video::downscaleBox(src, dst, 1, 1);   // 33 Mpx block: 32-bit accumulator would wrap
    EXPECT_EQ(dst.data[0], 255);
    video::Frame empty, d2;
    EXPECT_NO_THROW(video::downscaleBox(empty, d2, 4, 4));
    EXPECT_NO_THROW(video::downscaleBox(src, d2, 0, 4));
}

TEST(Switcher, WithoutSourcesIsSafe) {
    switcher::Switcher sw({});
    video::Frame f;
    video::prepareFrame(f, 8, 8);
    EXPECT_NO_THROW(sw.renderProgram(f, 0));
    EXPECT_NO_THROW(sw.renderPreview(f, 0));
    EXPECT_FALSE(sw.setPreview(0));
    EXPECT_FALSE(sw.cut());
    EXPECT_FALSE(sw.startDissolve(100));
}

TEST(Synthetic, NegativeAndHugeNumbersAreSafe) {
    for (int n : {-1, -17, 0, 10, 12345, -2147483647 - 1}) {
        video::SyntheticSource s(n, "x");
        video::Frame f;
        video::prepareFrame(f, 64, 36);
        EXPECT_NO_THROW(s.render(f, 7)) << n;
    }
}

TEST(Synthetic, CachedBackgroundGivesIdenticalFramesAndIsThreadSafe) {
    video::SyntheticSource a(3, "a");
    video::Frame f1, f2;
    video::prepareFrame(f1, 192, 108);
    video::prepareFrame(f2, 192, 108);
    a.render(f1, 12);     // builds the cache
    a.render(f2, 12);     // uses it
    EXPECT_EQ(f1.data, f2.data);
    std::vector<std::thread> ts;
    for (int t = 0; t < 4; ++t)
        ts.emplace_back([&, t] {
            video::Frame f;
            video::prepareFrame(f, 64 + t * 16, 36 + t * 9);
            for (int i = 0; i < 50; ++i) a.render(f, static_cast<std::uint64_t>(i));
        });
    for (auto& t : ts) t.join();
}
