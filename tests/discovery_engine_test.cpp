#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "discovery/engine.hpp"

using namespace hb::discovery;
using namespace std::chrono_literals;

namespace {

DiscoveredDevice dev(const std::string& id, const std::string& ip, Protocol p, const std::string& name = "", bool ptz = false) {
    DiscoveredDevice d;
    d.id = id;
    d.ip = ip;
    d.name = name;
    d.protocols = {p};
    d.ptz = ptz;
    return d;
}

class StaticSource : public IDiscoverySource {
public:
    StaticSource(std::string n, std::vector<DiscoveredDevice> d, std::string err = {}) : name_(std::move(n)), devs_(std::move(d)), err_(std::move(err)) {}
    std::string name() const override { return name_; }
    SourceResult discover(const CancelToken&) override { return {devs_, err_}; }

private:
    std::string name_;
    std::vector<DiscoveredDevice> devs_;
    std::string err_;
};

class ThrowingSource : public IDiscoverySource {
public:
    std::string name() const override { return "thrower"; }
    SourceResult discover(const CancelToken&) override { throw std::runtime_error("kaboom"); }
};

// Blocks until cancelled (or 3 s safety), then flags that it observed the cancellation.
class HangingSource : public IDiscoverySource {
public:
    std::string name() const override { return "hang"; }
    SourceResult discover(const CancelToken& c) override {
        for (int i = 0; i < 300 && !c.cancelled(); ++i) std::this_thread::sleep_for(10ms);
        sawCancel->store(c.cancelled());
        finished->store(true);
        SourceResult r;
        r.devices.push_back(dev("late", "9.9.9.9", Protocol::Rtsp));
        return r;
    }
    std::shared_ptr<std::atomic<bool>> sawCancel = std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<std::atomic<bool>> finished = std::make_shared<std::atomic<bool>>(false);
};

}  // namespace

TEST(Merge, DeduplicatesByIpAndId) {
    auto out = mergeDevices({dev("onvif:A", "192.168.1.64", Protocol::Onvif, "Hik", true), dev("ndi:cam1", "192.168.1.64", Protocol::Ndi, "NDI Cam"),
                             dev("onvif:A", "", Protocol::Onvif), dev("uvc:1", "", Protocol::Uvc, "USB"), dev("onvif:B", "192.168.1.65", Protocol::Onvif)});
    ASSERT_EQ(out.size(), 3u);
    EXPECT_EQ(out[0].id, "onvif:A");
    EXPECT_EQ(out[0].name, "Hik");
    EXPECT_EQ(out[0].protocols, (std::vector<Protocol>{Protocol::Onvif, Protocol::Ndi}));
    EXPECT_TRUE(out[0].ptz);
    EXPECT_EQ(out[1].id, "uvc:1");  // empty IPs never merge together
}

TEST(Merge, FillsMissingFields) {
    DiscoveredDevice a = dev("a", "1.1.1.1", Protocol::Onvif);
    a.name = "1.1.1.1";  // placeholder name equal to IP
    DiscoveredDevice b = dev("b", "1.1.1.1", Protocol::Ndi, "Real Name");
    b.manufacturer = "Acme";
    auto out = mergeDevices({a, b});
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].name, "Real Name");
    EXPECT_EQ(out[0].manufacturer, "Acme");
}

TEST(Engine, AggregatesSourcesAndDeduplicates) {
    DiscoveryEngine e;
    e.addSource(std::make_shared<StaticSource>("onvif", std::vector{dev("onvif:A", "10.0.0.1", Protocol::Onvif)}), 500ms);
    e.addSource(std::make_shared<StaticSource>("ndi", std::vector{dev("ndi:x", "10.0.0.1", Protocol::Ndi), dev("ndi:y", "10.0.0.2", Protocol::Ndi)}), 500ms);
    auto r = e.run();
    ASSERT_EQ(r.devices.size(), 2u);
    EXPECT_EQ(r.devices[0].protocols.size(), 2u);
    ASSERT_EQ(r.sources.size(), 2u);
    EXPECT_EQ(r.sources[0].status, SourceReport::Status::Ok);
    EXPECT_EQ(r.sources[1].deviceCount, 2u);
    EXPECT_EQ(r.devices[0].source, "onvif");
}

TEST(Engine, FailingThrowingAndHangingSourcesAreIsolated) {
    DiscoveryEngine e;
    auto hang = std::make_shared<HangingSource>();
    e.addSource(std::make_shared<StaticSource>("bad", std::vector<DiscoveredDevice>{}, "network down"), 500ms);
    e.addSource(std::make_shared<ThrowingSource>(), 500ms);
    e.addSource(hang, 100ms);
    e.addSource(std::make_shared<StaticSource>("good", std::vector{dev("onvif:G", "10.0.0.9", Protocol::Onvif)}), 500ms);
    const auto t0 = std::chrono::steady_clock::now();
    auto r = e.run();
    const auto took = std::chrono::steady_clock::now() - t0;

    ASSERT_EQ(r.sources.size(), 4u);
    EXPECT_EQ(r.sources[0].status, SourceReport::Status::Failed);
    EXPECT_EQ(r.sources[0].message, "network down");
    EXPECT_EQ(r.sources[1].status, SourceReport::Status::Exception);
    EXPECT_EQ(r.sources[1].message, "kaboom");
    EXPECT_EQ(r.sources[2].status, SourceReport::Status::TimedOut);
    EXPECT_EQ(r.sources[3].status, SourceReport::Status::Ok);
    ASSERT_EQ(r.devices.size(), 1u);  // the late device of the hanging source is discarded
    EXPECT_EQ(r.devices[0].id, "onvif:G");
    EXPECT_LT(took, 400ms);  // did not wait for the hanging source beyond its timeout

    // The abandoned source was told to cancel and winds down by itself.
    for (int i = 0; i < 100 && !hang->finished->load(); ++i) std::this_thread::sleep_for(10ms);
    EXPECT_TRUE(hang->finished->load());
    EXPECT_TRUE(hang->sawCancel->load());
}

TEST(Engine, SourcesRunInParallel) {
    class Sleepy : public IDiscoverySource {
    public:
        std::string name() const override { return "sleepy"; }
        SourceResult discover(const CancelToken&) override { std::this_thread::sleep_for(150ms); return {}; }
    };
    DiscoveryEngine e;
    for (int i = 0; i < 4; ++i) e.addSource(std::make_shared<Sleepy>(), 1000ms);
    const auto t0 = std::chrono::steady_clock::now();
    auto r = e.run();
    EXPECT_LT(std::chrono::steady_clock::now() - t0, 500ms);  // 4 x 150 ms serial would be 600 ms
    for (const auto& s : r.sources) EXPECT_EQ(s.status, SourceReport::Status::Ok);
}

TEST(Engine, EmptyEngineAndNullSource) {
    DiscoveryEngine e;
    e.addSource(nullptr, 10ms);
    auto r = e.run();
    EXPECT_TRUE(r.devices.empty());
    EXPECT_TRUE(r.sources.empty());
}
