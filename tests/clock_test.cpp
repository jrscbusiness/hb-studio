#include <gtest/gtest.h>

#include <thread>

#include "core/clock.hpp"

using namespace hb::core;
using namespace std::chrono;

TEST(Clock, DeadlineIsExactAndDriftFree) {
    const auto t0 = SteadyClock::time_point{};
    // 30 fps: tick 30 is exactly 1 s.
    EXPECT_EQ(tickDeadline(t0, {30, 1}, 30) - t0, seconds(1));
    // 29.97 (30000/1001): tick 30000 is exactly 1001 s -> no accumulated error.
    EXPECT_EQ(tickDeadline(t0, {30000, 1001}, 30000) - t0, seconds(1001));
    // Very long session: 10 days at 60 fps, still exact.
    const std::uint64_t n = 60ull * 86400 * 10;
    EXPECT_EQ(tickDeadline(t0, {60, 1}, n) - t0, hours(24 * 10));
}

TEST(Clock, DeliversRoughlyTheRequestedRate) {
    ProgramClock clock;
    std::atomic<int> n{0};
    clock.start({60, 1}, [&](auto, auto) { n.fetch_add(1); });
    std::this_thread::sleep_for(milliseconds(500));
    clock.stop();
    // ~30 expected; deliberately wide bounds: shared CI runners can stall for 100+ ms.
    EXPECT_GE(n.load(), 8);
    EXPECT_LE(n.load(), 32);
    EXPECT_FALSE(clock.running());
}

TEST(Clock, SlowCallbackSkipsInsteadOfBursting) {
    ProgramClock clock;
    std::vector<std::uint64_t> idx;
    clock.start({100, 1}, [&](std::uint64_t i, auto) {  // period 10 ms
        idx.push_back(i);
        std::this_thread::sleep_for(milliseconds(55));   // overruns ~5 periods
    });
    std::this_thread::sleep_for(milliseconds(400));
    clock.stop();
    ASSERT_GE(idx.size(), 3u);
    EXPECT_GT(clock.stats().skippedTicks, 0u);
    for (std::size_t k = 1; k < idx.size(); ++k) EXPECT_GT(idx[k], idx[k - 1] + 1);
}

TEST(Clock, ThrowingCallbackKeepsClockAlive) {
    ProgramClock clock;
    std::atomic<int> n{0};
    clock.start({100, 1}, [&](auto, auto) { n.fetch_add(1); throw std::runtime_error("x"); });
    std::this_thread::sleep_for(milliseconds(150));
    clock.stop();
    EXPECT_GE(n.load(), 5);
    EXPECT_EQ(clock.stats().callbackErrors, static_cast<std::uint64_t>(n.load()));
}

TEST(Clock, StopIsPromptEvenWithSlowFps) {
    ProgramClock clock;
    clock.start({1, 10}, [](auto, auto) {});  // one tick every 10 s
    const auto t = SteadyClock::now();
    clock.stop();
    EXPECT_LT(SteadyClock::now() - t, seconds(1));
}
