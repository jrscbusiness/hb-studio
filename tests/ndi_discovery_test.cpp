#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <thread>

#include "ndi/ndi.hpp"
#include "ndi_test_util.hpp"

using namespace hb::ndi;
using namespace hb::ndi::test;
using namespace std::chrono_literals;

namespace {
// The discovery thread polls at a fixed ~1s cadence (see discovery.cpp); tests that must observe
// an update wait a bit over that, same style as other polling tests in this codebase
// (tests/discovery_engine_test.cpp's HangingSource uses a comparable real-time safety margin).
constexpr auto kPollWait = 1500ms;
}  // namespace

TEST(NdiDiscovery, UnavailableBackendReportsItClearly) {
    auto backend = std::make_shared<FakeNdiBackend>();
    backend->available_ = false;
    NdiDiscovery disc(backend);
    EXPECT_FALSE(disc.available());
    EXPECT_FALSE(disc.unavailableMessage().empty());
    EXPECT_TRUE(disc.sources().empty());
}

TEST(NdiDiscovery, AvailableBackendHasNoUnavailableMessage) {
    auto backend = std::make_shared<FakeNdiBackend>();
    NdiDiscovery disc(backend);
    EXPECT_TRUE(disc.available());
    EXPECT_TRUE(disc.unavailableMessage().empty());
}

TEST(NdiDiscovery, DoesNotPollBeforeFirstSourcesCall) {
    auto backend = std::make_shared<FakeNdiBackend>();
    NdiDiscovery disc(backend);
    std::this_thread::sleep_for(300ms);
    EXPECT_EQ(backend->findCreateCalls.load(), 0) << "no background polling before sources() is first called";
}

TEST(NdiDiscovery, ReflectsWhatTheBackendReportsAndUpdatesOverTime) {
    auto backend = std::make_shared<FakeNdiBackend>();
    NdiDiscovery disc(backend);

    EXPECT_TRUE(disc.sources().empty());  // first call: nothing yet, but it must have started polling

    NdiSourceInfo a;
    a.name = "PC-1 (Camera 1)";
    a.urlAddress = "169.254.1.1:5961";
    backend->setSources({a});

    bool sawOne = false;
    const auto deadline = std::chrono::steady_clock::now() + kPollWait;
    while (std::chrono::steady_clock::now() < deadline) {
        auto list = disc.sources();
        if (list.size() == 1 && list[0].name == a.name && list[0].urlAddress == a.urlAddress) {
            sawOne = true;
            break;
        }
        std::this_thread::sleep_for(20ms);
    }
    EXPECT_TRUE(sawOne);

    backend->setSources({});
    bool sawEmpty = false;
    const auto deadline2 = std::chrono::steady_clock::now() + kPollWait;
    while (std::chrono::steady_clock::now() < deadline2) {
        if (disc.sources().empty()) {
            sawEmpty = true;
            break;
        }
        std::this_thread::sleep_for(20ms);
    }
    EXPECT_TRUE(sawEmpty);
}

TEST(NdiDiscovery, RetriesFindCreateUntilItSucceeds) {
    auto backend = std::make_shared<FakeNdiBackend>();
    backend->failFindCreate = true;
    NdiDiscovery disc(backend);
    EXPECT_TRUE(disc.sources().empty());
    std::this_thread::sleep_for(100ms);
    EXPECT_GE(backend->findCreateCalls.load(), 1);

    NdiSourceInfo a;
    a.name = "Cam";
    backend->setSources({a});
    backend->failFindCreate = false;

    bool ok = false;
    const auto deadline = std::chrono::steady_clock::now() + kPollWait * 2;
    while (std::chrono::steady_clock::now() < deadline) {
        if (disc.sources().size() == 1) {
            ok = true;
            break;
        }
        std::this_thread::sleep_for(20ms);
    }
    EXPECT_TRUE(ok);
}

TEST(NdiDiscovery, DestructorStopsTheThreadCleanly) {
    auto backend = std::make_shared<FakeNdiBackend>();
    {
        NdiDiscovery disc(backend);
        disc.sources();
        std::this_thread::sleep_for(50ms);
    }
    // No crash, no hang on destruction -- reaching this line is the assertion. findDestroy must
    // have been called exactly once per successful findCreate (no leak of the find handle).
    EXPECT_EQ(backend->findDestroyCalls.load(), backend->findCreateCalls.load() > 0 ? 1 : 0);
}

TEST(NdiDiscovery, MultipleInstancesAreIndependent) {
    auto backend = std::make_shared<FakeNdiBackend>();
    NdiDiscovery a(backend);
    NdiDiscovery b(backend);
    EXPECT_TRUE(a.sources().empty());
    EXPECT_TRUE(b.sources().empty());
}
