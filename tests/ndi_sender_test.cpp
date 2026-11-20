#include <gtest/gtest.h>

#include <vector>

#include "ndi/ndi.hpp"
#include "ndi_test_util.hpp"

using namespace hb::ndi;
using namespace hb::ndi::test;

namespace {
std::vector<std::uint8_t> frame(int w, int h, std::uint8_t b, std::uint8_t g, std::uint8_t r) {
    std::vector<std::uint8_t> v(static_cast<std::size_t>(w) * h * 4);
    for (std::size_t i = 0; i + 3 < v.size(); i += 4) { v[i] = b; v[i + 1] = g; v[i + 2] = r; v[i + 3] = 255; }
    return v;
}
}  // namespace

TEST(NdiSender, UnavailableBackendReportsItClearlyAndFeedIsANoOp) {
    auto backend = std::make_shared<FakeNdiBackend>();
    backend->available_ = false;
    NdiSender s("HB Studio Program", backend);
    EXPECT_FALSE(s.available());
    EXPECT_FALSE(s.unavailableMessage().empty());
    EXPECT_FALSE(s.active());
    auto f = frame(4, 2, 1, 2, 3);
    s.feed(f.data(), 4, 2, 16, 30, 1);
    EXPECT_FALSE(s.active());
    EXPECT_EQ(backend->sendCreateCalls.load(), 0);  // never even tried while unavailable
}

TEST(NdiSender, AvailableBackendSendsFramesLazily) {
    auto backend = std::make_shared<FakeNdiBackend>();
    NdiSender s("HB Studio Program", backend);
    EXPECT_TRUE(s.available());
    EXPECT_TRUE(s.unavailableMessage().empty());
    EXPECT_FALSE(s.active());                 // nothing created before the first feed()
    EXPECT_EQ(backend->sendCreateCalls.load(), 0);

    auto f = frame(8, 4, 10, 20, 30);
    s.feed(f.data(), 8, 4, 32, 60, 1);
    EXPECT_TRUE(s.active());
    EXPECT_EQ(backend->sendCreateCalls.load(), 1);
    EXPECT_EQ(backend->lastSendName(), "HB Studio Program");
    EXPECT_EQ(backend->sendVideoCalls.load(), 1);
    EXPECT_EQ(backend->lastSentWidth(), 8);
    EXPECT_EQ(backend->lastSentHeight(), 4);
    const auto px = backend->lastSentFirstPixel();
    ASSERT_EQ(px.size(), 4u);
    EXPECT_EQ(px[0], 10); EXPECT_EQ(px[1], 20); EXPECT_EQ(px[2], 30);

    s.feed(f.data(), 8, 4, 32, 60, 1);
    EXPECT_EQ(backend->sendCreateCalls.load(), 1);  // reused, not recreated
    EXPECT_EQ(backend->sendVideoCalls.load(), 2);
}

TEST(NdiSender, RetriesCreationOnEveryFeedUntilItSucceeds) {
    auto backend = std::make_shared<FakeNdiBackend>();
    backend->failSendCreate = true;
    NdiSender s("X", backend);
    auto f = frame(2, 2, 0, 0, 0);
    s.feed(f.data(), 2, 2, 8, 30, 1);
    EXPECT_FALSE(s.active());
    EXPECT_EQ(backend->sendCreateCalls.load(), 1);
    EXPECT_EQ(backend->sendVideoCalls.load(), 0);

    backend->failSendCreate = false;
    s.feed(f.data(), 2, 2, 8, 30, 1);
    EXPECT_TRUE(s.active());
    EXPECT_EQ(backend->sendCreateCalls.load(), 2);
    EXPECT_EQ(backend->sendVideoCalls.load(), 1);
}

TEST(NdiSender, HostileCallsNeverCrash) {
    auto backend = std::make_shared<FakeNdiBackend>();
    NdiSender s("X", backend);
    s.feed(nullptr, 10, 10, 40, 30, 1);
    EXPECT_FALSE(s.active());
    s.feed(reinterpret_cast<const std::uint8_t*>(1), 0, 10, 0, 30, 1);
    s.feed(reinterpret_cast<const std::uint8_t*>(1), 10, 0, 40, 30, 1);
    s.feed(reinterpret_cast<const std::uint8_t*>(1), -1, -1, -4, 30, 1);
    EXPECT_FALSE(s.active());
    EXPECT_EQ(backend->sendCreateCalls.load(), 0);
}

TEST(NdiSender, DestructorDestroysTheSendHandle) {
    auto backend = std::make_shared<FakeNdiBackend>();
    {
        NdiSender s("X", backend);
        auto f = frame(2, 2, 0, 0, 0);
        s.feed(f.data(), 2, 2, 8, 30, 1);
        EXPECT_TRUE(s.active());
    }
    EXPECT_EQ(backend->sendDestroyCalls.load(), 1);
}
