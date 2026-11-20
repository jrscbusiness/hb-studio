// Registry, parameter metadata and robustness of every built-in effect.
#include <gtest/gtest.h>

#include <limits>
#include <set>

#include "effects_test_util.hpp"

using namespace hb::audio;
using namespace hb::test;

namespace {
const char* kIds[] = {"hb.gain", "hb.phase", "hb.mono", "hb.delay", "hb.highpass", "hb.lowpass",
                      "hb.eq3", "hb.gate", "hb.compressor", "hb.limiter", "hb.deesser"};
}

TEST(EffectsRegistry, AllBuiltinsRegisteredWithCategoryAndSource) {
    registerBuiltinEffects();
    registerBuiltinEffects();  // idempotent
    auto list = EffectRegistry::instance().list();
    std::set<std::string> cats = {"Dinâmica", "Equalização", "Utilitários"};
    for (const char* id : kIds) {
        bool found = false;
        for (const auto& i : list)
            if (i.typeId == id) {
                found = true;
                EXPECT_EQ(i.source, "builtin") << id;
                EXPECT_TRUE(cats.count(i.category)) << id << " " << i.category;
                EXPECT_FALSE(i.name.empty());
            }
        EXPECT_TRUE(found) << id;
        auto e = EffectRegistry::instance().create(id);
        ASSERT_TRUE(e) << id;
        EXPECT_STREQ(e->typeId(), id);
    }
}

TEST(EffectsRegistry, ParamMetadataIsSane) {
    for (const char* id : kIds) {
        auto e = make(id);
        ASSERT_TRUE(e);
        std::set<std::string> seen;
        for (const auto& p : e->params()) {
            EXPECT_FALSE(p.id.empty());
            for (char c : p.id) {
                EXPECT_TRUE((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_') << id << "." << p.id;
            }
            EXPECT_TRUE(seen.insert(p.id).second) << "duplicate " << p.id;
            EXPECT_FALSE(p.label.empty());
            EXPECT_LT(p.min, p.max);
            EXPECT_GE(p.def, p.min);
            EXPECT_LE(p.def, p.max);
            if (p.logarithmic) {
                EXPECT_GT(p.min, 0.f);
            }
            if (!p.choices.empty()) {
                EXPECT_EQ(p.min, 0.f);
                EXPECT_EQ(p.max, static_cast<float>(p.choices.size() - 1));
            }
            EXPECT_FLOAT_EQ(e->getParam(p.id), p.def) << id << "." << p.id;
        }
        EXPECT_FALSE(e->params().empty());
    }
}

TEST(EffectsRegistry, SetParamClampsAndIgnoresUnknown) {
    auto e = make("hb.gain");
    e->setParam("gain_db", 100.f);
    EXPECT_FLOAT_EQ(e->getParam("gain_db"), 24.f);
    e->setParam("gain_db", -100.f);
    EXPECT_FLOAT_EQ(e->getParam("gain_db"), -24.f);
    e->setParam("gain_db", std::numeric_limits<float>::quiet_NaN());
    EXPECT_FLOAT_EQ(e->getParam("gain_db"), -24.f);
    e->setParam("nope", 1.f);
    EXPECT_FLOAT_EQ(e->getParam("nope"), 0.f);
    auto m = make("hb.mono");
    m->setParam("mode", 1.4f);
    EXPECT_FLOAT_EQ(m->getParam("mode"), 1.f);
    m->setParam("mode", 7.f);
    EXPECT_FLOAT_EQ(m->getParam("mode"), 2.f);
}

TEST(EffectsRobust, StableAtAllRatesAndChannelCountsWithExtremeParams) {
    const int rates[] = {44100, 48000, 96000};
    const int chans[] = {1, 2, 8};
    for (const char* id : kIds)
        for (int sr : rates)
            for (int ch : chans)
                for (int extreme = 0; extreme < 3; ++extreme) {
                    auto e = make(id, sr, ch);
                    ASSERT_TRUE(e);
                    for (const auto& p : e->params())
                        e->setParam(p.id, extreme == 0 ? p.def : (extreme == 1 ? p.min : p.max));
                    e->reset();
                    auto v = noise(static_cast<std::size_t>(sr / 2), ch, 1.0f, 7);
                    std::size_t pos = 0, frames = v.size() / static_cast<std::size_t>(ch), k = 0;
                    const std::size_t sizes[] = {1, 7, 31, 32, 33, 480, 1000};
                    while (pos < frames) {
                        const std::size_t n = std::min(sizes[k++ % 7], frames - pos);
                        e->process(v.data() + pos * static_cast<std::size_t>(ch), n);
                        pos += n;
                    }
                    for (float x : v)
                        ASSERT_TRUE(std::isfinite(x)) << id << " sr=" << sr << " ch=" << ch << " ex=" << extreme;
                    for (float x : v) ASSERT_LT(std::fabs(x), 100.f) << id;
                }
}

TEST(EffectsRobust, NanAndInfInputGiveFiniteOutputAndRecover) {
    for (const char* id : kIds) {
        auto e = make(id);
        auto v = noise(4800, 2, 0.5f, 3);
        v[10] = std::numeric_limits<float>::quiet_NaN();
        v[11] = std::numeric_limits<float>::infinity();
        v[500] = -std::numeric_limits<float>::infinity();
        v[1000] = std::numeric_limits<float>::max();
        e->process(v.data(), 2400);
        e->process(v.data() + 4800, 2400);
        for (float x : v) ASSERT_TRUE(std::isfinite(x)) << id;
        auto clean = noise(9600, 2, 0.5f, 4);
        e->process(clean.data(), 4800);
        e->process(clean.data() + 9600, 4800);
        for (float x : clean) ASSERT_TRUE(std::isfinite(x)) << id;
    }
}

TEST(EffectsRobust, ResetClearsState) {
    // Feed a loud burst, reset, then silence must come out as exact silence (no ringing left).
    for (const char* id : kIds) {
        auto e = make(id);
        auto burst = noise(4800, 2, 0.9f, 5);
        e->process(burst.data(), 4800);
        e->reset();
        std::vector<float> z(2 * 4800, 0.f);
        e->process(z.data(), 4800);
        for (float x : z) ASSERT_EQ(x, 0.f) << id;
    }
}

TEST(EffectsRobust, DenormalInputDoesNotProduceDenormalOutput) {
    for (const char* id : kIds) {
        auto e = make(id);
        std::vector<float> v(2 * 2000, 1e-40f);  // denormal floats
        e->process(v.data(), 2000);
        for (float x : v) ASSERT_TRUE(x == 0.f || std::fabs(x) >= 1e-30f) << id;
    }
}

TEST(EffectsRobust, ZeroFramesAndNullAreNoops) {
    auto e = EffectRegistry::instance().create("hb.compressor");
    registerBuiltinEffects();
    e = EffectRegistry::instance().create("hb.compressor");
    ASSERT_TRUE(e);
    float x[2] = {0.1f, 0.1f};
    e->process(x, 0);
    e->process(nullptr, 0);
    e->process(x, 1);
    EXPECT_TRUE(std::isfinite(x[0]));
}
