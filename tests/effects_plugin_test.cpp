// Plugin effect adapter: validation of untrusted descriptors and guarded processing.
// The test "plugin" is plain C-style functions filling hb_effects_v1 (no dlopen).
#include <gtest/gtest.h>

#include <cstring>
#include <limits>
#include <stdexcept>

#include "audio/plugin_effect.hpp"
#include "effects_test_util.hpp"

using namespace hb::audio;
using namespace hb::test;

namespace {

enum class Fault { None, NanOut, InfOut, Throw, NanFirstN };
struct Ctl {
    Fault fault = Fault::None;
    int badBlocksLeft = 0;
    int created = 0, destroyed = 0, resets = 0, processCalls = 0;
    float lastGain = 1.f;
    bool createFails = false;
    float seenInputMax = 0.f;
    // describe() misbehaviour switches
    int describeMode = 0;
    int count = 2;
    int describeRc = 0;
};
Ctl g;

void setStr(char* dst, std::size_t cap, const char* s) {
    std::memset(dst, 0, cap);
    std::strncpy(dst, s, cap - 1);
}

int tCount() { return g.count; }
int tDescribe(int index, hb_effect_desc* out, hb_effect_param* params, int) {
    if (g.describeRc) return g.describeRc;
    std::memset(out, 0, sizeof *out);
    setStr(out->type_id, sizeof out->type_id, index == 0 ? "test.gain" : "test.other");
    setStr(out->name, sizeof out->name, index == 0 ? "Ganho de teste" : "Outro");
    out->param_count = 1;
    setStr(params[0].id, sizeof params[0].id, "gain");
    setStr(params[0].label, sizeof params[0].label, "Ganho");
    setStr(params[0].unit, sizeof params[0].unit, "x");
    params[0].min_value = 0.f; params[0].max_value = 2.f; params[0].default_value = 1.f; params[0].logarithmic = 0;
    switch (g.describeMode) {
        case 1: std::memset(out->name, 'A', sizeof out->name); break;                       // no NUL
        case 2: setStr(out->type_id, sizeof out->type_id, "bad id!"); break;                 // illegal chars
        case 3: out->param_count = HB_EFFECT_MAX_PARAMS + 1; break;
        case 4: out->param_count = -1; break;
        case 5: params[0].min_value = 2.f; params[0].max_value = 2.f; break;                 // min == max
        case 6: params[0].default_value = 5.f; break;                                        // default outside
        case 7: setStr(params[0].id, sizeof params[0].id, "Gain"); break;                    // upper case id
        case 8: params[0].max_value = std::numeric_limits<float>::infinity(); break;
        case 9: params[0].min_value = std::numeric_limits<float>::quiet_NaN(); break;
        case 10: std::memset(params[0].label, 'x', sizeof params[0].label); break;           // label w/o NUL
        case 11: setStr(out->type_id, sizeof out->type_id, "test.gain"); break;              // dup (index 1 too)
        case 12: setStr(out->name, sizeof out->name, ""); break;
        case 13: out->param_count = 2; setStr(params[1].id, sizeof params[1].id, "gain");
                 setStr(params[1].label, sizeof params[1].label, "x");
                 params[1].min_value = 0; params[1].max_value = 1; params[1].default_value = 0; break;  // dup id
        case 14: setStr(out->name, sizeof out->name, "bad\x01name"); break;                  // control char
        case 15: setStr(out->name, sizeof out->name, "bad\xC3"); break;                      // truncated UTF-8
        case 16: params[0].logarithmic = 1; params[0].min_value = 0.f; break;                // log with min 0
        case 17: throw std::runtime_error("describe exploded");
        default: break;
    }
    if (index == 1 && g.describeMode == 11) setStr(out->type_id, sizeof out->type_id, "test.gain");
    return 0;
}
void* tCreate(int, int, int) {
    if (g.createFails) return nullptr;
    ++g.created;
    return new float(1.f);
}
void tSetParam(void* inst, int idx, float v) {
    if (idx == 0) { *static_cast<float*>(inst) = v; g.lastGain = v; }
}
void tProcess(void* inst, float* d, int frames) {
    ++g.processCalls;
    float mx = 0;
    for (int i = 0; i < frames * 2; ++i) mx = std::max(mx, std::fabs(d[i]));
    g.seenInputMax = mx;
    const bool bad = g.fault == Fault::NanOut || g.fault == Fault::InfOut ||
                     (g.fault == Fault::NanFirstN && g.badBlocksLeft > 0);
    if (g.fault == Fault::Throw) throw std::runtime_error("plugin exploded");
    if (g.fault == Fault::NanFirstN && g.badBlocksLeft > 0) --g.badBlocksLeft;
    const float k = *static_cast<float*>(inst);
    for (int i = 0; i < frames * 2; ++i) d[i] = d[i] * k;
    if (bad) d[1] = (g.fault == Fault::InfOut) ? std::numeric_limits<float>::infinity() : std::numeric_limits<float>::quiet_NaN();
}
void tReset(void*) { ++g.resets; }
void tDestroy(void* inst) { ++g.destroyed; delete static_cast<float*>(inst); }

std::shared_ptr<PluginEffectLibrary> makeLib(std::shared_ptr<void> keep = nullptr) {
    g = Ctl{};
    auto lib = std::make_shared<PluginEffectLibrary>();
    lib->api.struct_size = sizeof(hb_effects_v1);
    lib->api.api_version = HB_PLUGIN_API_VERSION;
    lib->api.count = tCount;
    lib->api.describe = tDescribe;
    lib->api.create = tCreate;
    lib->api.set_param = tSetParam;
    lib->api.process = tProcess;
    lib->api.reset = tReset;
    lib->api.destroy = tDestroy;
    lib->keepAlive = std::move(keep);
    return lib;
}

std::unique_ptr<Effect> firstEffect(const std::shared_ptr<PluginEffectLibrary>& lib) {
    std::vector<PluginEffectDesc> d;
    std::string err;
    EXPECT_TRUE(describePluginEffects(*lib, d, err)) << err;
    EXPECT_FALSE(d.empty());
    return createPluginEffect(lib, 0, d.at(0));
}

std::vector<float> ones(std::size_t frames, float v = 0.5f) { return std::vector<float>(frames * 2, v); }

}  // namespace

TEST(EffectsPluginDescribe, ValidDescriptorsAreConverted) {
    auto lib = makeLib();
    std::vector<PluginEffectDesc> d;
    std::string err;
    ASSERT_TRUE(describePluginEffects(*lib, d, err)) << err;
    ASSERT_EQ(d.size(), 2u);
    EXPECT_EQ(d[0].typeId, "test.gain");
    EXPECT_EQ(d[0].name, "Ganho de teste");
    ASSERT_EQ(d[0].params.size(), 1u);
    EXPECT_EQ(d[0].params[0].id, "gain");
    EXPECT_EQ(d[0].params[0].unit, "x");
    EXPECT_FLOAT_EQ(d[0].params[0].max, 2.f);
    EXPECT_FLOAT_EQ(d[0].params[0].def, 1.f);
    g.count = 0;
    EXPECT_TRUE(describePluginEffects(*lib, d, err));
    EXPECT_TRUE(d.empty());
}

TEST(EffectsPluginDescribe, RejectsEveryKindOfBadDescriptor) {
    for (int mode : {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17}) {
        auto lib = makeLib();
        g.describeMode = mode;
        std::vector<PluginEffectDesc> d;
        d.push_back({});   // must be cleared on failure
        std::string err;
        EXPECT_FALSE(describePluginEffects(*lib, d, err)) << "mode " << mode;
        EXPECT_FALSE(err.empty()) << "mode " << mode;
        EXPECT_TRUE(d.empty());
    }
}

TEST(EffectsPluginDescribe, RejectsBadCountsRcAndMissingFunctions) {
    std::vector<PluginEffectDesc> d;
    std::string err;
    for (int c : {-1, 65, 1000000}) {
        auto lib = makeLib();
        g.count = c;
        EXPECT_FALSE(describePluginEffects(*lib, d, err)) << c;
    }
    {
        auto lib = makeLib();
        g.count = 64;   // exactly the limit but two distinct ids only -> duplicate type ids
        EXPECT_FALSE(describePluginEffects(*lib, d, err));
    }
    {
        auto lib = makeLib();
        g.describeRc = -3;
        EXPECT_FALSE(describePluginEffects(*lib, d, err));
    }
    for (int k = 0; k < 7; ++k) {
        auto lib = makeLib();
        switch (k) {
            case 0: lib->api.count = nullptr; break;
            case 1: lib->api.describe = nullptr; break;
            case 2: lib->api.create = nullptr; break;
            case 3: lib->api.set_param = nullptr; break;
            case 4: lib->api.process = nullptr; break;
            case 5: lib->api.reset = nullptr; break;
            default: lib->api.destroy = nullptr; break;
        }
        EXPECT_FALSE(describePluginEffects(*lib, d, err)) << k;
    }
}

TEST(EffectsPluginEffect, ProcessesAndForwardsParams) {
    auto lib = makeLib();
    auto e = firstEffect(lib);
    ASSERT_TRUE(e);
    EXPECT_STREQ(e->typeId(), "test.gain");
    ASSERT_EQ(e->params().size(), 1u);
    auto v = ones(100);
    e->process(v.data(), 100);          // before prepare: untouched
    EXPECT_EQ(v[0], 0.5f);
    EXPECT_EQ(g.processCalls, 0);
    e->prepare(48000, 2);
    EXPECT_EQ(g.created, 1);
    e->setParam("gain", 1.5f);
    e->setParam("gain", 99.f);          // clamped to max 2
    EXPECT_FLOAT_EQ(e->getParam("gain"), 2.f);
    e->setParam("gain", 1.5f);
    e->setParam("zzz", 3.f);
    e->process(v.data(), 100);
    EXPECT_FLOAT_EQ(v[0], 0.75f);
    EXPECT_FLOAT_EQ(g.lastGain, 1.5f);
    e->reset();
    EXPECT_EQ(g.resets, 1);
    // re-prepare re-creates the instance and re-applies the stored parameter values
    e->prepare(44100, 2);
    EXPECT_EQ(g.created, 2);
    EXPECT_EQ(g.destroyed, 1);
    auto w = ones(10);
    e->process(w.data(), 10);
    EXPECT_FLOAT_EQ(w[0], 0.75f);
    e.reset();
    EXPECT_EQ(g.destroyed, 2);
}

TEST(EffectsPluginEffect, NanInputNeverReachesThePlugin) {
    auto lib = makeLib();
    auto e = firstEffect(lib);
    e->prepare(48000, 2);
    auto v = ones(64);
    v[5] = std::numeric_limits<float>::quiet_NaN();
    v[6] = std::numeric_limits<float>::infinity();
    e->process(v.data(), 64);
    EXPECT_TRUE(std::isfinite(g.seenInputMax));
    for (float x : v) EXPECT_TRUE(std::isfinite(x));
}

TEST(EffectsPluginEffect, NonFiniteOutputIsZeroedCountedThenBypassed) {
    for (Fault f : {Fault::NanOut, Fault::InfOut}) {
        auto lib = makeLib();
        auto e = firstEffect(lib);
        e->prepare(48000, 2);
        auto* pe = dynamic_cast<PluginEffect*>(e.get());
        ASSERT_NE(pe, nullptr);
        g.fault = f;
        for (int i = 0; i < kPluginMaxConsecutiveBadBlocks; ++i) {
            EXPECT_FALSE(pe->bypassed()) << i;
            auto v = ones(64);
            e->process(v.data(), 64);
            for (float x : v) ASSERT_EQ(x, 0.f);   // whole block zeroed
            EXPECT_EQ(pe->errorCount(), static_cast<std::uint64_t>(i + 1));
        }
        EXPECT_TRUE(pe->bypassed());
        // bypass: audio passes through untouched and the plugin is no longer called
        const int calls = g.processCalls;
        auto v = ones(64, 0.25f);
        e->process(v.data(), 64);
        EXPECT_EQ(g.processCalls, calls);
        for (float x : v) EXPECT_EQ(x, 0.25f);
        // prepare() gives it a fresh chance
        g.fault = Fault::None;
        e->prepare(48000, 2);
        EXPECT_FALSE(pe->bypassed());
        auto w = ones(8);
        e->process(w.data(), 8);
        EXPECT_EQ(w[0], 0.5f);
    }
}

TEST(EffectsPluginEffect, IsolatedBadBlocksDoNotTriggerBypass) {
    auto lib = makeLib();
    auto e = firstEffect(lib);
    e->prepare(48000, 2);
    auto* pe = dynamic_cast<PluginEffect*>(e.get());
    for (int round = 0; round < 5; ++round) {
        g.fault = Fault::NanFirstN;
        g.badBlocksLeft = kPluginMaxConsecutiveBadBlocks - 1;
        for (int i = 0; i < kPluginMaxConsecutiveBadBlocks - 1; ++i) { auto v = ones(16); e->process(v.data(), 16); }
        g.fault = Fault::None;
        auto v = ones(16);
        e->process(v.data(), 16);   // a good block resets the consecutive counter
        EXPECT_EQ(v[0], 0.5f);
    }
    EXPECT_FALSE(pe->bypassed());
    EXPECT_EQ(pe->errorCount(), static_cast<std::uint64_t>(5 * (kPluginMaxConsecutiveBadBlocks - 1)));
}

TEST(EffectsPluginEffect, ExceptionsAreContained) {
    auto lib = makeLib();
    auto e = firstEffect(lib);
    e->prepare(48000, 2);
    auto* pe = dynamic_cast<PluginEffect*>(e.get());
    g.fault = Fault::Throw;
    auto v = ones(32);
    EXPECT_NO_THROW(e->process(v.data(), 32));
    for (float x : v) EXPECT_EQ(x, 0.f);
    EXPECT_EQ(pe->errorCount(), 1u);
    for (int i = 0; i < kPluginMaxConsecutiveBadBlocks; ++i) e->process(v.data(), 32);
    EXPECT_TRUE(pe->bypassed());
}

TEST(EffectsPluginEffect, CreateFailureMeansBypassAndKeepAliveHoldsTheLibrary) {
    bool released = false;
    {
        std::shared_ptr<void> keep(new int(1), [&](void* p) { released = true; delete static_cast<int*>(p); });
        auto lib = makeLib(keep);
        keep.reset();
        EXPECT_FALSE(released);
        g.createFails = true;
        auto e = firstEffect(lib);
        lib.reset();                 // the effect alone keeps the library alive
        EXPECT_FALSE(released);
        e->prepare(48000, 2);
        auto* pe = dynamic_cast<PluginEffect*>(e.get());
        EXPECT_TRUE(pe->bypassed());
        EXPECT_EQ(pe->errorCount(), 1u);
        auto v = ones(16);
        e->process(v.data(), 16);
        EXPECT_EQ(v[0], 0.5f);
        e.reset();
    }
    EXPECT_TRUE(released);
}

TEST(EffectsPluginEffect, WorksThroughRegistryWithPluginSource) {
    auto lib = makeLib();
    std::vector<PluginEffectDesc> d;
    std::string err;
    ASSERT_TRUE(describePluginEffects(*lib, d, err));
    auto& reg = EffectRegistry::instance();
    for (std::size_t i = 0; i < d.size(); ++i) {
        const auto desc = d[i];
        EXPECT_TRUE(reg.add({desc.typeId, desc.name, "Plugin", "plug.test"},
                            [lib, i, desc] { return createPluginEffect(lib, static_cast<int>(i), desc); }));
    }
    auto e = reg.create("test.gain");
    ASSERT_TRUE(e);
    e->prepare(48000, 2);
    e->setParam("gain", 2.f);
    auto v = ones(4, 0.25f);
    e->process(v.data(), 4);
    EXPECT_FLOAT_EQ(v[0], 0.5f);
    e.reset();
    reg.removeBySource("plug.test");
    EXPECT_FALSE(reg.contains("test.gain"));
}

TEST(EffectsPluginEffect, NullLibraryOrBadIndexGivesNull) {
    EXPECT_EQ(createPluginEffect(nullptr, 0, PluginEffectDesc{}), nullptr);
    auto lib = makeLib();
    EXPECT_EQ(createPluginEffect(lib, -1, PluginEffectDesc{}), nullptr);
}
