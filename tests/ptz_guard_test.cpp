#include <gtest/gtest.h>

#include "ptz/guarded_controller.hpp"

using namespace hb::ptz;
using namespace std::chrono_literals;

namespace {

// Recording provider with configurable failures.
class RecProvider : public PTZProvider {
public:
    std::vector<std::string> log;
    std::vector<MoveVector> drives;
    PTZCapabilities caps;
    bool failStop = false;
    int stopCalls = 0;

    RecProvider() {
        caps.panTilt = caps.zoom = caps.presetRecall = caps.presetSave = caps.presetDelete = true;
        caps.focusDrive = caps.focusAuto = caps.iris = true;
        caps.maxPresetId = 64;
    }
    std::string name() const override { return "rec"; }
    const PTZCapabilities& capabilities() const override { return caps; }
    PtzResult connect() override { log.push_back("connect"); return PtzResult::success(); }
    PtzResult disconnect() override { log.push_back("disconnect"); return PtzResult::success(); }
    PtzResult drive(const MoveVector& v) override {
        drives.push_back(v);
        log.push_back("drive");
        return PtzResult::success();
    }
    PtzResult stopAll() override {
        ++stopCalls;
        log.push_back("stop");
        return failStop ? PtzResult::timeout("stop failed") : PtzResult::success();
    }
    PtzResult stopLens() override { log.push_back("stoplens"); return PtzResult::success(); }
    PtzResult gotoPreset(int id) override { log.push_back("goto" + std::to_string(id)); return PtzResult::success(); }
    PtzResult savePreset(int id) override { log.push_back("save" + std::to_string(id)); return PtzResult::success(); }
    PtzResult deletePreset(int id) override { log.push_back("del" + std::to_string(id)); return PtzResult::success(); }
    PtzReply<std::vector<PresetInfo>> getPresets() override { return {PtzResult::notSupported(), {}}; }
    PtzResult focusAuto() override { log.push_back("af"); return PtzResult::success(); }
    PtzResult focusManual() override { return PtzResult::notSupported(); }
    PtzResult focusNear() override { log.push_back("near"); return PtzResult::success(); }
    PtzResult focusFar() override { log.push_back("far"); return PtzResult::success(); }
    PtzResult irisOpen() override { log.push_back("iris+"); return PtzResult::success(); }
    PtzResult irisClose() override { log.push_back("iris-"); return PtzResult::success(); }
    PtzReply<CameraStatus> getCameraStatus() override { return {PtzResult::notSupported(), {}}; }
};

struct Fx {
    std::shared_ptr<ManualClock> clock = std::make_shared<ManualClock>();
    RecProvider* prov = new RecProvider;
    std::vector<std::string> errors;
    std::unique_ptr<GuardedController> g;
    explicit Fx(GuardConfig cfg = {}) {
        g = std::make_unique<GuardedController>(std::unique_ptr<PTZProvider>(prov), clock, cfg,
                                                [this](const std::string& op, const PtzResult&) { errors.push_back(op); });
        g->connect();
    }
    int count(const std::string& s) const {
        int n = 0;
        for (auto& l : prov->log) n += l == s;
        return n;
    }
};

}  // namespace

TEST(Guard, CallsAreNonBlockingAndOnlyRecordIntent) {
    Fx f;
    EXPECT_TRUE(f.g->panLeft().ok());
    EXPECT_TRUE(f.g->gotoPreset(3).ok());
    EXPECT_EQ(f.count("drive"), 0);  // nothing sent until tick()
    EXPECT_EQ(f.count("goto3"), 0);
    f.g->tick();
    EXPECT_EQ(f.count("drive"), 1);
}

TEST(Guard, RateLimitedTo20Hz) {
    Fx f(GuardConfig{.maxRateHz = 1000.0});  // request absurd rate: clamped to 20 Hz
    for (int i = 0; i < 10; ++i) f.g->gotoPreset(i);
    int sent = 0;
    // 1 second of ticks at 1 kHz.
    for (int ms = 0; ms < 1000; ++ms) {
        f.g->tick();
        f.clock->advance(1ms);
    }
    for (auto& l : f.prov->log) sent += l.rfind("goto", 0) == 0;
    EXPECT_EQ(sent, 10);
    // 10 commands at 20 Hz take >= 450 ms: check spacing by replaying with a counter per 50 ms window.
    Fx g2;
    for (int i = 0; i < 40; ++i) g2.g->gotoPreset(i % 30);
    int total = 0;
    for (int ms = 0; ms < 500; ++ms) {
        g2.g->tick();
        g2.clock->advance(1ms);
    }
    for (auto& l : g2.prov->log) total += l.rfind("goto", 0) == 0;
    EXPECT_LE(total, 11);  // 20 Hz * 0.5 s (+1 for the first, immediate slot)
    EXPECT_GE(total, 9);
}

TEST(Guard, MovementCoalescesToLatestIntent) {
    Fx f;
    f.g->panLeft();
    f.g->tiltUp();
    f.g->panRight();
    f.g->tick();
    ASSERT_EQ(f.prov->drives.size(), 1u);
    EXPECT_DOUBLE_EQ(f.prov->drives[0].pan, 0.5);
    EXPECT_DOUBLE_EQ(f.prov->drives[0].tilt, 0.5);
}

TEST(Guard, SpeedMapsIntoVector) {
    Fx f;
    f.g->setPanSpeed(1.0);
    f.g->setTiltSpeed(0.2);
    f.g->panLeft();
    f.g->tiltDown();
    f.g->tick();
    EXPECT_DOUBLE_EQ(f.prov->drives.back().pan, -1.0);
    EXPECT_DOUBLE_EQ(f.prov->drives.back().tilt, -0.2);
}

TEST(Guard, WatchdogStopsWhenMovementNotRenewed) {
    Fx f;
    f.g->panLeft();
    f.g->tick();
    for (int i = 0; i < 4; ++i) {  // renew every 100 ms for 400 ms
        f.clock->advance(100ms);
        f.g->panLeft();
        f.g->tick();
    }
    EXPECT_EQ(f.count("stop"), 0);
    f.clock->advance(499ms);
    f.g->tick();
    EXPECT_EQ(f.count("stop"), 0);
    f.clock->advance(2ms);  // 501 ms since last renewal
    f.g->tick();
    EXPECT_EQ(f.count("stop"), 1);
    EXPECT_EQ(f.g->stats().watchdogStops, 1u);
    // Does not keep stopping once stopped.
    f.clock->advance(1s);
    f.g->tick();
    EXPECT_EQ(f.count("stop"), 1);
}

TEST(Guard, WatchdogAlsoCoversFocusDrive) {
    Fx f;
    f.g->focusNear();
    f.g->tick();
    EXPECT_EQ(f.count("near"), 1);
    f.clock->advance(600ms);
    f.g->tick();
    EXPECT_EQ(f.count("stop"), 1);
    EXPECT_EQ(f.count("stoplens"), 1);
}

TEST(Guard, StopBypassesRateLimiter) {
    Fx f;
    f.g->gotoPreset(1);
    f.g->tick();  // consumes the slot
    f.g->panLeft();
    f.g->stop();
    f.g->tick();  // same instant: Stop must go out immediately
    EXPECT_EQ(f.count("stop"), 1);
    EXPECT_EQ(f.count("drive"), 0);  // pending movement was cancelled
}

TEST(Guard, FailedStopIsRetriedAtRateLimit) {
    Fx f;
    f.prov->failStop = true;
    f.g->panLeft();
    f.g->tick();
    f.g->stop();
    f.g->tick();
    EXPECT_EQ(f.prov->stopCalls, 1);
    f.g->tick();  // same instant: retry waits for the limiter
    EXPECT_EQ(f.prov->stopCalls, 1);
    f.clock->advance(50ms);
    f.g->tick();
    EXPECT_EQ(f.prov->stopCalls, 2);
    f.prov->failStop = false;
    f.clock->advance(50ms);
    f.g->tick();
    EXPECT_EQ(f.prov->stopCalls, 3);
    f.clock->advance(50ms);
    f.g->tick();
    EXPECT_EQ(f.prov->stopCalls, 3);  // success: no more retries
    EXPECT_FALSE(f.errors.empty());   // failures were reported
}

TEST(Guard, DisconnectSendsStopFirst) {
    Fx f;
    f.g->panLeft();
    f.g->tick();
    f.prov->log.clear();
    EXPECT_TRUE(f.g->disconnect().ok());
    ASSERT_GE(f.prov->log.size(), 2u);
    EXPECT_EQ(f.prov->log[0], "stop");
    EXPECT_EQ(f.prov->log.back(), "disconnect");
    EXPECT_EQ(f.g->panLeft().status, PtzStatus::Error);
}

TEST(Guard, UnsupportedAndQueueFull) {
    Fx f(GuardConfig{.maxQueue = 3});
    f.prov->caps.iris = false;
    EXPECT_EQ(f.g->irisOpen().status, PtzStatus::NotSupported);
    f.prov->caps.focusManual = false;
    EXPECT_EQ(f.g->focusManual().status, PtzStatus::NotSupported);
    for (int i = 0; i < 3; ++i) EXPECT_TRUE(f.g->gotoPreset(i).ok());
    EXPECT_EQ(f.g->gotoPreset(9).status, PtzStatus::Error);
    EXPECT_EQ(f.g->stats().droppedQueueFull, 1u);
}

TEST(Guard, ProviderFailureNeverThrowsAndIsReported) {
    struct Thrower : RecProvider {
        PtzResult gotoPreset(int) override { throw std::runtime_error("boom"); }
    };
    auto clock = std::make_shared<ManualClock>();
    std::vector<std::string> errs;
    GuardedController g(std::make_unique<Thrower>(), clock, {}, [&](const std::string& op, const PtzResult& r) {
        errs.push_back(op + ":" + statusName(r.status));
    });
    g.connect();
    g.gotoPreset(1);
    EXPECT_NO_THROW(g.tick());
    ASSERT_EQ(errs.size(), 1u);
    EXPECT_EQ(errs[0], "gotoPreset:Error");
}

TEST(Guard, ErrorCallbackThrowingIsContained) {
    auto clock = std::make_shared<ManualClock>();
    auto* p = new RecProvider;
    p->failStop = true;
    GuardedController g(std::unique_ptr<PTZProvider>(p), clock, {}, [](const std::string&, const PtzResult&) {
        throw std::runtime_error("ui bug");
    });
    g.connect();
    g.panLeft();
    g.tick();
    g.stop();
    EXPECT_NO_THROW(g.tick());
}
