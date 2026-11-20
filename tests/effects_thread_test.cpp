// setParam() from another thread while process() runs (meant to be run under ThreadSanitizer too).
#include <gtest/gtest.h>

#include <atomic>
#include <random>
#include <thread>

#include "effects_test_util.hpp"

using namespace hb::audio;
using namespace hb::test;

TEST(EffectsThreads, ParamChangesWhileProcessingAreSafeAndFinite) {
    const char* ids[] = {"hb.gain", "hb.phase", "hb.mono", "hb.delay", "hb.highpass", "hb.lowpass",
                         "hb.eq3", "hb.gate", "hb.compressor", "hb.limiter", "hb.deesser"};
    for (const char* id : ids) {
        auto e = make(id, 48000, 2);
        ASSERT_TRUE(e) << id;
        std::atomic<bool> stop{false};
        std::thread setter([&] {
            std::mt19937 g(42);
            const auto& ps = e->params();
            while (!stop.load()) {
                for (const auto& p : ps) {
                    std::uniform_real_distribution<float> d(p.min, p.max);
                    e->setParam(p.id, d(g));
                    (void)e->getParam(p.id);
                }
                (void)e->gainReductionDb();
            }
        });
        bool finite = true;
        double maxAbs = 0;
        for (int blk = 0; blk < 400; ++blk) {
            auto v = noise(480, 2, 0.8f, static_cast<unsigned>(blk));
            e->process(v.data(), 480);
            for (float x : v) {
                if (!std::isfinite(x)) finite = false;
                maxAbs = std::max(maxAbs, static_cast<double>(std::fabs(x)));
            }
        }
        stop = true;
        setter.join();
        EXPECT_TRUE(finite) << id;
        EXPECT_LT(maxAbs, 50.0) << id;
    }
}

TEST(EffectsThreads, LimiterStillRespectsCeilingWhileGainParamsChange) {
    auto lim = make("hb.limiter", 48000, 2);
    lim->setParam("ceiling_db", -2.f);
    std::atomic<bool> stop{false};
    std::thread setter([&] {
        std::mt19937 g(1);
        std::uniform_real_distribution<float> d(10.f, 1000.f);
        while (!stop.load()) lim->setParam("release_ms", d(g));
    });
    double m = 0;
    for (int blk = 0; blk < 300; ++blk) {
        auto v = noise(480, 2, 5.0f, static_cast<unsigned>(blk));
        lim->process(v.data(), 480);
        for (float x : v) m = std::max(m, static_cast<double>(std::fabs(x)));
    }
    stop = true;
    setter.join();
    EXPECT_LE(m, std::pow(10.0, -2.0 / 20.0) + 1e-7);
}
