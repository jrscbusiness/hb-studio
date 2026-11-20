// Real-time guarantees: process()/setParam() never allocate. This translation unit replaces the global
// operator new/delete of the whole test binary and counts allocations while `g_counting` is set.
#include <gtest/gtest.h>

#include <atomic>
#include <cstdlib>
#include <memory>
#include <new>

#include "effects_test_util.hpp"

namespace {
std::atomic<long> g_allocs{0};
std::atomic<bool> g_counting{false};

struct CountScope {
    CountScope() { g_allocs = 0; g_counting = true; }
    ~CountScope() { g_counting = false; }
    long count() const { return g_allocs.load(); }
};
}  // namespace

void* operator new(std::size_t n) {
    if (g_counting.load(std::memory_order_relaxed)) g_allocs.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return operator new(n); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
    if (g_counting.load(std::memory_order_relaxed)) g_allocs.fetch_add(1, std::memory_order_relaxed);
    return std::malloc(n ? n : 1);
}
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept { return operator new(n, std::nothrow); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }

using namespace hb::audio;
using namespace hb::test;

TEST(EffectsRealtime, AllocationCounterActuallyWorks) {
    CountScope s;
    int* volatile p = new int[16];
    delete[] p;
    EXPECT_GE(s.count(), 1);
}

TEST(EffectsRealtime, ProcessAndSetParamNeverAllocate) {
    const char* ids[] = {"hb.gain", "hb.phase", "hb.mono", "hb.delay", "hb.highpass", "hb.lowpass",
                         "hb.eq3", "hb.gate", "hb.compressor", "hb.limiter", "hb.deesser"};
    for (const char* id : ids)
        for (int sr : {44100, 96000})
            for (int ch : {1, 2, 8}) {
                auto e = make(id, sr, ch);
                ASSERT_TRUE(e) << id;
                auto v = noise(static_cast<std::size_t>(sr / 10), ch, 1.0f, 5);
                // pre-built strings so that the test itself does not allocate inside the counted window
                std::vector<std::string> names;
                for (const auto& p : e->params()) names.push_back(p.id);
                const std::string unknown = "unknown_param_id_longer_than_sso_buffer_to_be_sure";
                v[3] = std::numeric_limits<float>::quiet_NaN();
                CountScope scope;
                for (int round = 0; round < 6; ++round) {
                    for (std::size_t k = 0; k < names.size(); ++k) {
                        const auto& info = e->params()[k];
                        e->setParam(names[k], round % 2 ? info.min : info.max);
                        (void)e->getParam(names[k]);
                    }
                    e->setParam(unknown, 1.f);
                    e->process(v.data(), v.size() / static_cast<std::size_t>(ch));
                    e->process(v.data(), 1);
                    e->process(v.data(), 33);
                    (void)e->gainReductionDb();
                }
                e->reset();
                e->process(v.data(), v.size() / static_cast<std::size_t>(ch));
                EXPECT_EQ(scope.count(), 0) << id << " sr=" << sr << " ch=" << ch;
            }
}
