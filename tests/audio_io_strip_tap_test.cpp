// FEATURE: multitrack - per-strip taps (pre/post fader) used by the ISO audio recorder.
#include <gtest/gtest.h>

#include <cmath>

#include "audio_io_test_util.hpp"

using namespace hb::audio_io;
using namespace hb::audio_io::test;

namespace {
void ext(Rig& r, int strip, std::shared_ptr<hb::audio::AudioSource> src) {
    const std::string id = "tap" + std::to_string(strip);
    ASSERT_TRUE(r.engine->registerExternalSource(id, std::move(src)));
    ASSERT_TRUE(r.engine->setStripInput(strip, "external", id));
}
}  // namespace

TEST(StripTap, PreFaderIgnoresFaderAndMute) {
    Rig r;
    ext(r, 0, std::make_shared<ConstSource>(0.5f, 0.25f));
    std::vector<float> got;
    const auto id = r.engine->addStripConsumer(0, false, [&](const float* p, std::size_t n) {
        EXPECT_EQ(n, kBlockFrames);
        got.assign(p, p + n * 2);
    });
    ASSERT_NE(id, 0u);
    r.engine->setStripParam(0, "faderDb", -60);
    r.engine->setStripParam(0, "mute", 1);
    r.run(10);
    ASSERT_EQ(got.size(), kBlockFrames * 2);
    EXPECT_NEAR(got[0], 0.5f, 1e-5f);
    EXPECT_NEAR(got[1], 0.25f, 1e-5f);
    r.engine->removeStripConsumer(id);
    got.clear();
    r.run(2);
    EXPECT_TRUE(got.empty());   // synchronous removal: never called again
}

TEST(StripTap, PostFaderFollowsFaderMuteAndSolo) {
    Rig r;
    ext(r, 0, std::make_shared<ConstSource>(0.5f, 0.5f));
    ext(r, 1, std::make_shared<ConstSource>(0.5f, 0.5f));
    float last = 0;
    r.engine->addStripConsumer(0, true, [&](const float* p, std::size_t n) { last = p[2 * (n - 1)]; });
    r.run(10);
    EXPECT_NEAR(last, 0.5f, 1e-4f);                       // 0 dB
    r.engine->setStripParam(0, "faderDb", -6.0206);
    r.run(10);
    EXPECT_NEAR(last, 0.25f, 1e-3f);
    r.engine->setStripParam(0, "faderDb", 0);
    r.engine->setStripParam(0, "mute", 1);
    r.run(10);
    EXPECT_NEAR(last, 0.0f, 1e-6f);
    r.engine->setStripParam(0, "mute", 0);
    r.engine->setStripParam(1, "solo", 1);                // another strip soloed: strip 0 is gated
    r.run(10);
    EXPECT_NEAR(last, 0.0f, 1e-6f);
}

TEST(StripTap, BadArgumentsAndThrowingConsumerAreHarmless) {
    Rig r;
    EXPECT_EQ(r.engine->addStripConsumer(-1, false, [](const float*, std::size_t) {}), 0u);
    EXPECT_EQ(r.engine->addStripConsumer(99, false, [](const float*, std::size_t) {}), 0u);
    EXPECT_EQ(r.engine->addStripConsumer(0, false, nullptr), 0u);
    r.engine->addStripConsumer(0, false, [](const float*, std::size_t) { throw 1; });
    int calls = 0;
    r.engine->addStripConsumer(0, false, [&](const float*, std::size_t) { ++calls; });
    r.run(3);
    EXPECT_EQ(calls, 3);
    EXPECT_GE(r.engine->stats().consumerErrors, 3u);
}
