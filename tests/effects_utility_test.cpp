// hb.gain, hb.phase, hb.mono, hb.delay
#include <gtest/gtest.h>

#include "effects_test_util.hpp"

using namespace hb::audio;
using namespace hb::test;

TEST(EffectsGain, ExactGainOverWholeRangeAllChannelCounts) {
    for (int ch : {1, 2, 8})
        for (float g : {-24.f, -12.f, -6.f, 0.f, 3.f, 6.f, 24.f}) {
            auto e = make("hb.gain", 48000, ch);
            e->setParam("gain_db", g);
            e->reset();
            auto v = noise(2000, ch, 0.5f, 9);
            auto ref = v;
            run(*e, v, ch, 77);
            const float lin = std::pow(10.f, g / 20.f);
            for (std::size_t i = 0; i < v.size(); ++i) ASSERT_NEAR(v[i], ref[i] * lin, 1e-5f * std::fabs(lin)) << g;
        }
}

TEST(EffectsGain, ChangeIsRampedNoZipper) {
    auto e = make("hb.gain");
    std::vector<float> v(2 * 9600, 0.01f);
    e->process(v.data(), 480);
    e->setParam("gain_db", 24.f);
    e->process(v.data() + 960, 9600 - 480);
    double maxStep = 0;
    for (std::size_t i = 1; i < 9600; ++i) maxStep = std::max(maxStep, static_cast<double>(std::fabs(v[2 * i] - v[2 * (i - 1)])));
    EXPECT_LT(maxStep, 0.002);                   // final value is 0.158; an unramped step would be 0.148
    EXPECT_NEAR(v.back(), 0.01f * 15.8489f, 1e-4);  // converged
}

TEST(EffectsPhase, InvertsExactlyAndSwitchesSmoothly) {
    auto e = make("hb.phase", 48000, 2);
    auto v = noise(1000, 2, 0.7f, 2);
    auto ref = v;
    run(*e, v, 2);
    for (std::size_t i = 0; i < v.size(); ++i) ASSERT_EQ(v[i], ref[i]);   // normal polarity: bit exact
    e->setParam("invert", 1.f);
    auto w = sine(1000.0, 0.5, 48000, 4800, 2);
    auto wr = w;
    run(*e, w, 2, 64);
    for (std::size_t i = 4800 - 100; i < 4800; ++i) ASSERT_NEAR(w[2 * i], -wr[2 * i], 1e-6);
    double maxStep = 0;
    for (std::size_t i = 1; i < 4800; ++i) maxStep = std::max(maxStep, static_cast<double>(std::fabs(w[2 * i] - w[2 * (i - 1)])));
    EXPECT_LT(maxStep, 0.2);   // no hard flip from +x to -x (which would jump by up to 1.0)
    // reset snaps to the current polarity
    e->reset();
    auto z = sine(1000.0, 0.5, 48000, 100, 2);
    auto zr = z;
    run(*e, z, 2);
    for (std::size_t i = 0; i < z.size(); ++i) ASSERT_EQ(z[i], -zr[i]);
}

TEST(EffectsMono, ModesProduceExpectedMix) {
    struct Case { float mode, expect; };
    for (Case c : {Case{0.f, 0.75f}, Case{1.f, 1.0f}, Case{2.f, 0.5f}}) {
        auto e = make("hb.mono");
        e->setParam("mode", c.mode);
        e->reset();
        std::vector<float> v(2 * 500);
        for (std::size_t i = 0; i < 500; ++i) { v[2 * i] = 1.0f; v[2 * i + 1] = 0.5f; }
        run(*e, v, 2, 50);
        for (std::size_t i = 0; i < v.size(); ++i) ASSERT_NEAR(v[i], c.expect, 1e-6f) << c.mode;
    }
}

TEST(EffectsMono, SingleChannelUntouchedAndExtraChannelsUntouched) {
    auto m1 = make("hb.mono", 48000, 1);
    m1->setParam("mode", 1.f);
    m1->reset();
    auto v = noise(300, 1, 0.5f, 3);
    auto r = v;
    run(*m1, v, 1);
    EXPECT_EQ(v, r);
    auto m8 = make("hb.mono", 48000, 4);
    m8->reset();
    auto w = noise(300, 4, 0.5f, 3);
    auto wr = w;
    run(*m8, w, 4);
    for (std::size_t i = 0; i < 300; ++i) {
        EXPECT_NEAR(w[4 * i], 0.5f * (wr[4 * i] + wr[4 * i + 1]), 1e-6);
        EXPECT_EQ(w[4 * i + 2], wr[4 * i + 2]);
        EXPECT_EQ(w[4 * i + 3], wr[4 * i + 3]);
    }
}

TEST(EffectsMono, SwitchModeIsSmooth) {
    auto e = make("hb.mono");
    e->setParam("mode", 1.f);
    e->reset();
    std::vector<float> v(2 * 4800);
    for (std::size_t i = 0; i < 4800; ++i) { v[2 * i] = 1.f; v[2 * i + 1] = -1.f; }
    e->process(v.data(), 960);
    e->setParam("mode", 2.f);
    e->process(v.data() + 1920, 4800 - 960);
    double maxStep = 0;
    for (std::size_t i = 1; i < 4800; ++i) maxStep = std::max(maxStep, static_cast<double>(std::fabs(v[2 * i] - v[2 * (i - 1)])));
    EXPECT_LT(maxStep, 0.25);   // an unsmoothed switch would jump by 2.0
    EXPECT_NEAR(v[2 * 4799], -1.f, 1e-3);
}

TEST(EffectsDelay, ExactDelayInSamplesAtAllRates) {
    for (int sr : {44100, 48000, 96000})
        for (float ms : {0.f, 1.f, 10.f, 123.4f, 2000.f}) {
            auto e = make("hb.delay", sr, 2);
            e->setParam("time_ms", ms);
            e->reset();
            const std::size_t D = static_cast<std::size_t>(std::floor(ms * sr / 1000.0 + 0.5));
            const std::size_t N = D + 3000;
            std::vector<float> v(2 * N, 0.f);
            v[2 * 100] = 1.f;
            v[2 * 100 + 1] = -0.5f;
            run(*e, v, 2, 997);
            for (std::size_t i = 0; i < N; ++i) {
                const float l = (i == 100 + D) ? 1.f : 0.f, r = (i == 100 + D) ? -0.5f : 0.f;
                ASSERT_EQ(v[2 * i], l) << "sr=" << sr << " ms=" << ms << " i=" << i;
                ASSERT_EQ(v[2 * i + 1], r);
            }
        }
}

TEST(EffectsDelay, EightChannelsAndMono) {
    for (int ch : {1, 8}) {
        auto e = make("hb.delay", 48000, ch);
        e->setParam("time_ms", 5.f);
        e->reset();
        std::vector<float> v(static_cast<std::size_t>(ch) * 1000, 0.f);
        for (int c = 0; c < ch; ++c) v[50 * static_cast<std::size_t>(ch) + static_cast<std::size_t>(c)] = static_cast<float>(c + 1);
        run(*e, v, ch, 64);
        for (int c = 0; c < ch; ++c)
            EXPECT_EQ(v[(50 + 240) * static_cast<std::size_t>(ch) + static_cast<std::size_t>(c)], static_cast<float>(c + 1));
    }
}

TEST(EffectsDelay, ChangingTimeDoesNotClickAndSettlesToExactDelay) {
    const int sr = 48000;
    auto e = make("hb.delay", sr, 2);
    auto in = sine(1000.0, 0.5, sr, sr, 2);
    auto v = in;
    double maxStep = 0;
    float prev = 0;
    const double times[] = {0, 25, 25, 5, 700, 0};
    for (std::size_t blk = 0; blk < 100; ++blk) {
        if (blk % 15 == 0) e->setParam("time_ms", static_cast<float>(times[(blk / 15) % 6]));
        e->process(v.data() + blk * 480 * 2, 480);
        for (std::size_t i = blk * 480; i < (blk + 1) * 480; ++i) {
            maxStep = std::max(maxStep, static_cast<double>(std::fabs(v[2 * i] - prev)));
            prev = v[2 * i];
        }
    }
    // an unfaded jump between taps could be as large as 1.0; the cross-fade keeps it near the sine slope
    EXPECT_LT(maxStep, 0.25);
    // final delay (0 ms after 90 blocks) is exact again
    for (std::size_t i = 47000; i < 48000; ++i) ASSERT_NEAR(v[2 * i], in[2 * i], 1e-6) << i;
}
