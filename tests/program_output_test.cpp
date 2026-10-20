#include <gtest/gtest.h>

#include <thread>

#include "video/program_output.hpp"

using namespace hb;
using namespace std::chrono;

namespace {
video::ProgramFormat small() { return {320, 180, {60, 1}}; }
}

TEST(ProgramOutput, ProducesFixedResolutionFramesWithoutAnyWindow) {
    video::ProgramOutput out(small());
    out.start();
    std::this_thread::sleep_for(milliseconds(200));
    out.stop();
    auto f = out.latest();
    ASSERT_TRUE(f);
    EXPECT_EQ(f->width, 320);
    EXPECT_EQ(f->height, 180);
    EXPECT_EQ(f->data.size(), 320u * 180u * 4u);
    EXPECT_GT(out.framesProduced(), 5u);
}

TEST(ProgramOutput, TestPatternHasBarsAndMovingMarker) {
    video::Frame f;
    f.width = 280; f.height = 100; f.stride = 280 * 4; f.data.resize(f.stride * f.height);
    video::renderTestPattern(f, 0);
    auto px = [&](int x, int y) { return f.data.data() + y * f.stride + x * 4; };
    EXPECT_EQ(px(0, 0)[2], 235);    // first bar white -> R high
    EXPECT_EQ(px(279, 0)[0], 235);  // last bar blue (B high in BGRA)
    EXPECT_EQ(px(279, 0)[2], 16);
    const std::vector<std::uint8_t> a = f.data;
    video::renderTestPattern(f, 10);
    EXPECT_NE(a, f.data);           // marker moved
}

TEST(ProgramOutput, ConsumersReceiveFramesAndFaultyOnesAreIsolated) {
    video::ProgramOutput out(small());
    std::atomic<int> good{0};
    out.addConsumer([](const auto&) { throw std::runtime_error("bad consumer"); });
    out.addConsumer([&](const auto&) { good.fetch_add(1); });
    out.start();
    std::this_thread::sleep_for(milliseconds(200));
    out.stop();
    EXPECT_GT(good.load(), 5);
    EXPECT_EQ(out.consumerErrors(), static_cast<std::uint64_t>(good.load()));
}

TEST(ProgramOutput, RemovedConsumerStopsReceiving) {
    video::ProgramOutput out(small());
    std::atomic<int> n{0};
    auto id = out.addConsumer([&](const auto&) { n.fetch_add(1); });
    out.start();
    std::this_thread::sleep_for(milliseconds(100));
    out.removeConsumer(id);
    const int at = n.load();
    std::this_thread::sleep_for(milliseconds(100));
    out.stop();
    EXPECT_LE(n.load(), at + 1);
}

TEST(ProgramOutput, ThrowingRendererKeepsOutputAlive) {
    video::ProgramOutput out(small());
    std::atomic<int> calls{0};
    out.setRenderer([&](video::Frame& f, std::uint64_t i) {
        if (calls.fetch_add(1) >= 3) throw std::runtime_error("bad renderer");
        video::renderTestPattern(f, i);
    });
    out.start();
    std::this_thread::sleep_for(milliseconds(200));
    out.stop();
    EXPECT_GT(calls.load(), 5);        // clock kept running
    EXPECT_TRUE(out.latest());         // last good frame still available
    EXPECT_GT(out.rendererErrors(), 0u);
    EXPECT_EQ(out.consumerErrors(), 0u);
}

// Regression (REVIEW-1 M6): consumers must KEEP receiving frames while the renderer fails.
TEST(ProgramOutput, ConsumersKeepReceivingFramesWhenRendererThrows) {
    video::ProgramOutput out(small());
    std::atomic<int> calls{0}, got{0};
    out.setRenderer([&](video::Frame& f, std::uint64_t i) {
        if (calls.fetch_add(1) >= 3) throw std::runtime_error("bad renderer");
        video::renderTestPattern(f, i);
    });
    out.addConsumer([&](const auto&) { got.fetch_add(1); });
    out.start();
    std::this_thread::sleep_for(milliseconds(300));
    out.stop();
    EXPECT_GT(out.rendererErrors(), 3u);
    EXPECT_GT(out.repeatedFrames(), 3u);
    EXPECT_GE(got.load(), calls.load() - 2);   // every tick reached the consumer
}

TEST(ProgramOutput, FirstFrameFailureSendsBlackNotGarbage) {
    video::ProgramOutput out(small());
    out.setRenderer([](video::Frame& f, std::uint64_t) { f.data.assign(f.data.size(), 0xAB); throw std::runtime_error("x"); });
    std::shared_ptr<const video::Frame> first;
    std::mutex m;
    out.addConsumer([&](const auto& f) { std::lock_guard l(m); if (!first) first = f; });
    out.start();
    std::this_thread::sleep_for(milliseconds(100));
    out.stop();
    std::lock_guard l(m);
    ASSERT_TRUE(first);
    for (auto b : first->data) ASSERT_EQ(b, 0);
}

// Regression (REVIEW-1 A5): removeConsumer must wait for a callback that is already running.
TEST(ProgramOutput, RemoveConsumerWaitsForInFlightCallback) {
    video::ProgramOutput out(small());
    std::atomic<bool> inside{false}, finished{false};
    auto id = out.addConsumer([&](const auto&) {
        inside = true;
        std::this_thread::sleep_for(milliseconds(150));
        finished = true;
    });
    out.start();
    while (!inside) std::this_thread::sleep_for(milliseconds(1));
    out.removeConsumer(id);
    EXPECT_TRUE(finished.load());      // returned only after the running callback completed
    const bool again = finished.exchange(false);
    (void)again;
    std::this_thread::sleep_for(milliseconds(100));
    out.stop();
    EXPECT_FALSE(finished.load());     // and it never ran again
}

TEST(ProgramOutput, ConsumerMayRemoveItselfWithoutDeadlock) {
    video::ProgramOutput out(small());
    std::atomic<int> calls{0};
    video::ProgramOutput::ConsumerId id = 0;
    id = out.addConsumer([&](const auto&) { calls.fetch_add(1); out.removeConsumer(id); });
    out.start();
    std::this_thread::sleep_for(milliseconds(150));
    out.stop();
    EXPECT_EQ(calls.load(), 1);
}

TEST(ProgramOutput, InvalidFpsDoesNotStartNorAnnounce) {
    core::EventBus bus;
    std::vector<std::string> topics;
    bus.subscribe("*", [&](const core::Event& e) { topics.push_back(e.topic); });
    video::ProgramOutput out({64, 36, {0, 1}}, &bus);
    out.start();
    EXPECT_FALSE(out.running());
    EXPECT_TRUE(topics.empty());
}

TEST(ProgramOutput, PublishesLifecycleEvents) {
    core::EventBus bus;
    std::vector<std::string> topics;
    bus.subscribe("program.*", [&](const core::Event& e) { topics.push_back(e.topic); });
    video::ProgramOutput out(small(), &bus);
    out.start();
    out.stop();
    ASSERT_EQ(topics.size(), 2u);
    EXPECT_EQ(topics[0], "program.started");
    EXPECT_EQ(topics[1], "program.stopped");
}

TEST(FramePool, RecyclesBuffersAndFramesMayOutliveThePool) {
    std::shared_ptr<video::Frame> keep;
    {
        video::FramePool pool(64, 32);
        auto a = pool.acquire();
        const void* addr = a->data.data();
        a.reset();
        EXPECT_EQ(pool.idleCount(), 1u);
        auto b = pool.acquire();
        EXPECT_EQ(b->data.data(), addr);  // same buffer reused
        keep = b;
    }
    EXPECT_EQ(keep->data.size(), 64u * 32u * 4u);  // still valid after pool destroyed
}
