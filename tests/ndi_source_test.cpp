#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <thread>

#include "ndi/ndi.hpp"
#include "ndi_test_util.hpp"
#include "video/frame_ops.hpp"
#include "video/source.hpp"

using namespace hb;
using namespace hb::ndi;
using namespace hb::ndi::test;
using namespace std::chrono_literals;

namespace {

NdiSourceInfo makeSource(std::string name = "Fonte de Teste") {
    NdiSourceInfo s;
    s.name = std::move(name);
    s.urlAddress = "169.254.9.9:5961";
    return s;
}

video::Frame frame(int w = 16, int h = 9) {
    video::Frame f;
    video::prepareFrame(f, w, h);
    return f;
}

template <class Pred>
bool waitUntil(Pred pred, std::chrono::milliseconds timeout = 2000ms) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(5ms);
    }
    return pred();
}

}  // namespace

TEST(NdiSource, GoesFromConnectingToOnlineWhenFramesArrive) {
    auto backend = std::make_shared<FakeNdiBackend>();
    backend->callsBeforeDelivering = 2;
    NdiSource src(makeSource(), backend);
    EXPECT_EQ(src.status().state, video::SourceStatus::State::Connecting);
    src.start();

    ASSERT_TRUE(waitUntil([&] { return src.status().state == video::SourceStatus::State::Online; }));
    auto st = src.status();
    EXPECT_EQ(st.width, backend->frameWidth.load());
    EXPECT_EQ(st.height, backend->frameHeight.load());

    auto f = frame();
    src.render(f, 0);
    EXPECT_EQ(f.data[0], 10);
    EXPECT_EQ(f.data[1], 20);
    EXPECT_EQ(f.data[2], 30);
    EXPECT_EQ(f.data[3], 255);
    src.stop();
}

TEST(NdiSource, RetriesWhenReceiverCannotBeCreated) {
    // A source that is simply off right now (or the network hiccupped) must not become a
    // permanent dead end: NdiSource keeps calling recvCreate in the background instead of giving
    // up after the first failure.
    auto backend = std::make_shared<FakeNdiBackend>();
    backend->failRecvCreate = true;
    NdiSource src(makeSource(), backend);
    src.start();
    ASSERT_TRUE(waitUntil([&] { return !src.status().message.empty(); }));
    EXPECT_NE(src.status().state, video::SourceStatus::State::Error);
    const int callsSoFar = backend->recvCreateCalls.load();
    ASSERT_TRUE(waitUntil([&] { return backend->recvCreateCalls.load() > callsSoFar; }, 3000ms))
        << "must keep retrying recvCreate rather than giving up permanently";
    EXPECT_NE(src.status().state, video::SourceStatus::State::Error);
    // render() must still be safe and never block/throw even though the source never connected.
    auto f = frame();
    EXPECT_NO_THROW(src.render(f, 0));
    src.stop();
}

TEST(NdiSource, UnavailableBackendGivesAClearError) {
    auto backend = std::make_shared<FakeNdiBackend>();
    backend->available_ = false;
    NdiSource src(makeSource(), backend);
    src.start();
    ASSERT_TRUE(waitUntil([&] { return src.status().state == video::SourceStatus::State::Error; }));
    src.stop();
}

TEST(NdiSource, HardErrorFromBackendTriggersReconnectAndKeepsRetrying) {
    // A hard error from one capture call used to be treated as terminal for the whole source.
    // It's now treated as "this receiver instance is dead": NdiSource destroys it and creates a
    // fresh one instead of leaving the camera stuck forever.
    auto backend = std::make_shared<FakeNdiBackend>();
    backend->mode = 2;  // hard error from the very first capture call
    NdiSource src(makeSource(), backend);
    src.start();
    ASSERT_TRUE(waitUntil([&] { return backend->recvDestroyCalls.load() >= 1; }, 3000ms));
    EXPECT_NE(src.status().state, video::SourceStatus::State::Error);
    EXPECT_FALSE(src.status().message.empty());
    const int recreatesSoFar = backend->recvCreateCalls.load();
    ASSERT_TRUE(waitUntil([&] { return backend->recvCreateCalls.load() > recreatesSoFar; }, 3000ms))
        << "must keep reconnecting rather than giving up after one hard error";
    auto f = frame();
    EXPECT_NO_THROW(src.render(f, 0));
    src.stop();
}

TEST(NdiSource, PerpetualTimeoutsNeverBlockRenderOrThrow) {
    auto backend = std::make_shared<FakeNdiBackend>();
    backend->mode = 1;  // always times out, never an error
    NdiSource src(makeSource(), backend);
    src.start();
    std::this_thread::sleep_for(200ms);
    auto f = frame();
    for (int i = 0; i < 50; ++i) EXPECT_NO_THROW(src.render(f, static_cast<std::uint64_t>(i)));
    auto st = src.status();
    EXPECT_NE(st.state, video::SourceStatus::State::Error);
    src.stop();
}

TEST(NdiSource, InconsistentFrameSizesAreRejectedNeverCrashOrGarbage) {
    auto backend = std::make_shared<FakeNdiBackend>();
    backend->mode = 3;  // hostile: declared size never matches the actual buffer
    NdiSource src(makeSource(), backend);
    src.start();
    std::this_thread::sleep_for(300ms);
    auto f = frame();
    for (int i = 0; i < 20; ++i) EXPECT_NO_THROW(src.render(f, static_cast<std::uint64_t>(i)));
    // Every call delivered a mismatched buffer, so NdiSource must never have accepted one: still
    // no signal, never "Online" off a corrupt frame.
    EXPECT_NE(src.status().state, video::SourceStatus::State::Online);
    EXPECT_GT(backend->framesDelivered.load(), 0);  // the fake really did try to hand frames over
    src.stop();
}

TEST(NdiSource, NoSignalAfterSourceStopsDeliveringFrames) {
    auto backend = std::make_shared<FakeNdiBackend>();
    NdiSource src(makeSource(), backend);
    src.start();
    ASSERT_TRUE(waitUntil([&] { return src.status().state == video::SourceStatus::State::Online; }));
    backend->mode = 1;  // now goes silent
    ASSERT_TRUE(waitUntil([&] { return src.status().state == video::SourceStatus::State::NoSignal; }, 3000ms));
    src.stop();
}

TEST(NdiSource, StartStopIsIdempotentAndLeavesNoThreadRunning) {
    auto backend = std::make_shared<FakeNdiBackend>();
    NdiSource src(makeSource(), backend);
    src.start();
    src.start();  // second start is a no-op while already running
    ASSERT_TRUE(waitUntil([&] { return src.status().state == video::SourceStatus::State::Online; }));
    src.stop();
    src.stop();  // idempotent
    EXPECT_EQ(backend->recvDestroyCalls.load(), 1);

    // restart cleanly
    src.start();
    ASSERT_TRUE(waitUntil([&] { return src.status().state == video::SourceStatus::State::Online; }));
    src.stop();
    EXPECT_EQ(backend->recvCreateCalls.load(), 2);
    EXPECT_EQ(backend->recvDestroyCalls.load(), 2);
}

TEST(NdiSource, DestructorStopsCleanlyWithoutLeakingTheThread) {
    auto backend = std::make_shared<FakeNdiBackend>();
    {
        NdiSource src(makeSource(), backend);
        src.start();
        std::this_thread::sleep_for(50ms);
    }
    EXPECT_EQ(backend->recvCreateCalls.load(), backend->recvDestroyCalls.load());
}

TEST(NdiSource, NameMatchesTheRequestedSource) {
    auto backend = std::make_shared<FakeNdiBackend>();
    NdiSource src(makeSource("Minha Câmera"), backend);
    EXPECT_EQ(src.name(), "Minha Câmera");
}

TEST(NdiSource, RenderBeforeStartIsSafeAndProducesAPlaceholder) {
    auto backend = std::make_shared<FakeNdiBackend>();
    NdiSource src(makeSource(), backend);
    auto f = frame();
    EXPECT_NO_THROW(src.render(f, 0));
    EXPECT_EQ(src.status().state, video::SourceStatus::State::Connecting);
}
