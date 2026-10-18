#include <gtest/gtest.h>

#include <thread>

#include "core/event_bus.hpp"

using namespace hb::core;

TEST(EventBus, PatternMatching) {
    EXPECT_TRUE(EventBus::matches("*", "anything.here"));
    EXPECT_TRUE(EventBus::matches("camera.*", "camera.connected"));
    EXPECT_TRUE(EventBus::matches("camera.*", "camera.frame.dropped"));
    EXPECT_FALSE(EventBus::matches("camera.*", "cameras.connected"));
    EXPECT_FALSE(EventBus::matches("camera.*", "camera"));
    EXPECT_TRUE(EventBus::matches("ptz.presetChanged", "ptz.presetChanged"));
    EXPECT_FALSE(EventBus::matches("ptz.presetChanged", "ptz.positionChanged"));
}

TEST(EventBus, DeliversOnlyToMatchingSubscribers) {
    EventBus bus;
    int cam = 0, sw = 0, all = 0;
    bus.subscribe("camera.*", [&](const Event&) { ++cam; });
    bus.subscribe("switcher.programChanged", [&](const Event&) { ++sw; });
    bus.subscribe("*", [&](const Event&) { ++all; });
    bus.publish({"camera.connected", {{"id", "cam01"}}});
    bus.publish({"switcher.programChanged", {}});
    EXPECT_EQ(cam, 1);
    EXPECT_EQ(sw, 1);
    EXPECT_EQ(all, 2);
}

TEST(EventBus, ThrowingHandlerDoesNotAffectOthers) {
    EventBus bus;
    int ran = 0;
    bus.subscribe("camera.error", [](const Event&) { throw std::runtime_error("boom"); });
    bus.subscribe("camera.error", [&](const Event&) { ++ran; });
    EXPECT_EQ(bus.publish({"camera.error", {}}), 1u);
    EXPECT_EQ(ran, 1);
    EXPECT_EQ(bus.handlerErrors(), 1u);
}

TEST(EventBus, UnsubscribeAndReentrancy) {
    EventBus bus;
    int n = 0;
    SubscriptionId id = 0;
    id = bus.subscribe("a", [&](const Event&) {
        ++n;
        bus.unsubscribe(id);                 // unsubscribe from inside handler
        bus.publish({"b", {}});              // publish from inside handler
    });
    bus.subscribe("b", [&](const Event&) { ++n; });
    bus.publish({"a", {}});
    bus.publish({"a", {}});
    EXPECT_EQ(n, 2);  // "a" handler once, "b" handler once
}

TEST(EventBus, ConcurrentPublishIsSafe) {
    EventBus bus;
    std::atomic<int> n{0};
    bus.subscribe("*", [&](const Event&) { n.fetch_add(1); });
    std::vector<std::thread> ts;
    for (int t = 0; t < 4; ++t)
        ts.emplace_back([&] { for (int i = 0; i < 1000; ++i) bus.publish({"x", {}}); });
    for (auto& t : ts) t.join();
    EXPECT_EQ(n.load(), 4000);
}
