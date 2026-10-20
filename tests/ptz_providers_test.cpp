#include <gtest/gtest.h>

#include "ptz/fake_transport.hpp"
#include "ptz/onvif_provider.hpp"
#include "ptz/pelco_provider.hpp"
#include "ptz/provider_selection.hpp"
#include "ptz/visca_provider.hpp"

using namespace hb::ptz;

namespace {

// Fake VISCA-over-IP camera: ACKs commands, answers position inquiries.
void ipCamera(const Bytes& d, FakeTransport& t) {
    auto f = visca::unwrapIp(d);
    if (!f) return;
    if (f->type == 0x0200) {
        t.pushIncoming(visca::wrapIp(visca::IpType::ControlReply, 0, {0x01}));
    } else if (f->type == 0x0100) {
        t.pushIncoming(visca::wrapIp(visca::IpType::Reply, f->sequence, {0x90, 0x41, 0xFF}));
    } else if (f->type == 0x0110) {
        if (f->payload.size() > 3 && f->payload[2] == 0x06)
            t.pushIncoming(visca::wrapIp(visca::IpType::Reply, f->sequence,
                                         {0x90, 0x50, 0x0F, 0x0F, 0x0F, 0x0E, 0x00, 0x00, 0x01, 0x00, 0xFF}));
        else
            t.pushIncoming(visca::wrapIp(visca::IpType::Reply, f->sequence,
                                         {0x90, 0x50, 0x01, 0x02, 0x03, 0x04, 0xFF}));
    }
}

struct ViscaFixture {
    FakeTransport* ft = new FakeTransport;
    std::unique_ptr<ViscaProvider> p;
    explicit ViscaFixture(ViscaConfig cfg = {}) {
        ft->onSend = ipCamera;
        p = std::make_unique<ViscaProvider>(std::unique_ptr<ITransport>(ft), cfg);
    }
    Bytes payload(std::size_t i) const { return visca::unwrapIp(ft->sent.at(i))->payload; }
};

}  // namespace

TEST(ViscaProvider, ConnectSendsSequenceResetThenUsesIncreasingSequence) {
    ViscaFixture f;
    ASSERT_TRUE(f.p->connect().ok());
    ASSERT_EQ(f.ft->sent.size(), 1u);
    EXPECT_EQ(visca::unwrapIp(f.ft->sent[0])->type, 0x0200);
    ASSERT_TRUE(f.p->panLeft().ok());
    ASSERT_TRUE(f.p->tiltUp().ok());
    auto a = visca::unwrapIp(f.ft->sent[1]);
    auto b = visca::unwrapIp(f.ft->sent[2]);
    EXPECT_EQ(b->sequence, a->sequence + 1);
}

TEST(ViscaProvider, PanTiltCombinesAxesAndUsesSpeeds) {
    ViscaFixture f;
    ASSERT_TRUE(f.p->connect().ok());
    f.p->setPanSpeed(1.0);
    f.p->setTiltSpeed(1.0);
    f.p->panLeft();
    f.p->tiltUp();
    EXPECT_EQ(f.payload(f.ft->sent.size() - 1), (Bytes{0x81, 0x01, 0x06, 0x01, 0x18, 0x14, 0x01, 0x01, 0xFF}));
    f.p->stop();
    // Stop sends pan/tilt stop then zoom stop.
    const auto n = f.ft->sent.size();
    EXPECT_EQ(f.payload(n - 2), visca::panTiltStop(1));
    EXPECT_EQ(f.payload(n - 1), visca::zoomDrive(1, 0));
}

TEST(ViscaProvider, DoesNotResendUnchangedState) {
    ViscaFixture f;
    f.p->connect();
    f.p->panLeft();
    const auto n = f.ft->sent.size();
    f.p->panLeft();
    EXPECT_EQ(f.ft->sent.size(), n);
}

TEST(ViscaProvider, PresetRangeIsValidated) {
    ViscaConfig c;
    c.maxPresetId = 15;
    ViscaFixture f(c);
    f.p->connect();
    EXPECT_TRUE(f.p->gotoPreset(15).ok());
    EXPECT_EQ(f.p->gotoPreset(16).status, PtzStatus::Error);
    EXPECT_EQ(f.p->savePreset(-1).status, PtzStatus::Error);
    EXPECT_TRUE(f.p->deletePreset(3).ok());
    EXPECT_EQ(f.payload(f.ft->sent.size() - 1), visca::presetReset(1, 3));
}

TEST(ViscaProvider, TimeoutWhenCameraSilent) {
    ViscaFixture f;
    f.ft->onSend = nullptr;
    f.p->connect();
    EXPECT_EQ(f.p->panLeft().status, PtzStatus::Timeout);
}

TEST(ViscaProvider, ErrorReplyMapsToError) {
    ViscaFixture f;
    f.ft->onSend = [](const Bytes& d, FakeTransport& t) {
        auto fr = visca::unwrapIp(d);
        if (fr && fr->type == 0x0100)
            t.pushIncoming(visca::wrapIp(visca::IpType::Reply, fr->sequence, {0x90, 0x60, 0x41, 0xFF}));
    };
    f.p->connect();
    auto r = f.p->gotoPreset(1);
    EXPECT_EQ(r.status, PtzStatus::Error);
    EXPECT_NE(r.detail.find("not executable"), std::string::npos);
}

TEST(ViscaProvider, SkipsStaleCompletionAndGarbage) {
    ViscaFixture f;
    f.ft->onSend = [](const Bytes& d, FakeTransport& t) {
        auto fr = visca::unwrapIp(d);
        if (fr && fr->type == 0x0100) {
            t.pushIncoming(Bytes{1, 2, 3});  // garbage datagram
            t.pushIncoming(visca::wrapIp(visca::IpType::Reply, 9, {0x90, 0x51, 0xFF}));  // old completion
            t.pushIncoming(visca::wrapIp(visca::IpType::Reply, fr->sequence, {0x90, 0x41, 0xFF}));
        }
    };
    f.p->connect();
    EXPECT_TRUE(f.p->panLeft().ok());
}

TEST(ViscaProvider, NotConnectedAndTransportFailuresAreResultsNotExceptions) {
    ViscaFixture f;
    EXPECT_EQ(f.p->panLeft().status, PtzStatus::Error);
    f.ft->failOpen = true;
    EXPECT_EQ(f.p->connect().status, PtzStatus::Error);
    f.ft->failOpen = false;
    f.p->connect();
    f.ft->failSend = true;
    EXPECT_EQ(f.p->panLeft().status, PtzStatus::Error);
}

TEST(ViscaProvider, StatusInquiry) {
    ViscaFixture f;
    f.p->connect();
    auto s = f.p->getCameraStatus();
    ASSERT_TRUE(s.ok());
    ASSERT_TRUE(s.value.position);
    EXPECT_EQ(s.value.position->pan, -2);
    EXPECT_EQ(s.value.position->tilt, 16);
    EXPECT_EQ(s.value.position->zoom, 0x1234);
}

TEST(ViscaProvider, DisconnectSendsStopFirst) {
    ViscaFixture f;
    f.p->connect();
    f.p->panRight();
    f.p->disconnect();
    // The last frames sent before closing are the stop commands.
    ASSERT_GE(f.ft->sent.size(), 2u);
    EXPECT_EQ(f.payload(f.ft->sent.size() - 1), visca::zoomDrive(1, 0));
    EXPECT_FALSE(f.ft->isOpen());
}

TEST(ViscaProvider, FocusDriveIsStoppedByStop) {
    ViscaFixture f;
    f.p->connect();
    ASSERT_TRUE(f.p->focusNear().ok());
    f.p->stop();
    EXPECT_EQ(f.payload(f.ft->sent.size() - 1), visca::focusStop(1));
}

TEST(ViscaProvider, SerialModeHasNoIpHeaderAndReassemblesStream) {
    ViscaConfig c;
    c.overIp = false;
    c.address = 2;
    auto* ft = new FakeTransport;
    ft->onSend = [](const Bytes&, FakeTransport& t) {
        t.pushIncoming(Bytes{0xA0});          // partial
        t.pushIncoming(Bytes{0x41, 0xFF});    // rest of ACK (camera addr 2 -> A0)
    };
    ViscaProvider p{std::unique_ptr<ITransport>(ft), c};
    p.connect();
    EXPECT_TRUE(p.panLeft().ok());
    EXPECT_EQ(ft->sent.back()[0], 0x82);
}

TEST(ViscaProvider, CapabilitiesDeclareWhatIsSupported) {
    ViscaFixture f;
    const auto& c = f.p->capabilities();
    EXPECT_TRUE(c.panTilt && c.zoom && c.focusAuto && c.iris && c.presetRecall);
    EXPECT_FALSE(c.presetList);
    EXPECT_EQ(f.p->getPresets().result.status, PtzStatus::NotSupported);
}

// ---------------- Pelco ----------------
TEST(PelcoProvider, SendsFramesAndStops) {
    auto* ft = new FakeTransport;
    PelcoProvider p{std::unique_ptr<ITransport>(ft), PelcoConfig{PelcoVariant::D, 1, 255}};
    ASSERT_TRUE(p.connect().ok());
    p.setPanSpeed(1.0);
    ASSERT_TRUE(p.panLeft().ok());
    EXPECT_EQ(ft->sent.back(), (Bytes{0xFF, 0x01, 0x00, 0x04, 0x3F, 0x00, 0x44}));
    p.stop();
    EXPECT_EQ(ft->sent.back(), (Bytes{0xFF, 0x01, 0x00, 0x00, 0x00, 0x00, 0x01}));
    ASSERT_TRUE(p.gotoPreset(5).ok());
    EXPECT_EQ(ft->sent.back()[3], 0x07);
    EXPECT_EQ(p.focusAuto().status, PtzStatus::NotSupported);
    EXPECT_EQ(p.getCameraStatus().result.status, PtzStatus::NotSupported);
    EXPECT_EQ(p.getPresets().result.status, PtzStatus::NotSupported);
    EXPECT_EQ(p.gotoPreset(300).status, PtzStatus::Error);
}

TEST(PelcoProvider, PelcoPUsesStxEtx) {
    auto* ft = new FakeTransport;
    PelcoProvider p{std::unique_ptr<ITransport>(ft), PelcoConfig{PelcoVariant::P, 1, 255}};
    p.connect();
    p.tiltUp();
    EXPECT_EQ(ft->sent.back()[0], 0xA0);
    EXPECT_EQ(ft->sent.back()[6], 0xAF);
}

TEST(PelcoProvider, DisconnectStopsBeforeClose) {
    auto* ft = new FakeTransport;
    PelcoProvider p{std::unique_ptr<ITransport>(ft), {}};
    p.connect();
    p.zoomIn();
    p.disconnect();
    EXPECT_EQ(ft->sent.back()[3], 0x00);
    EXPECT_EQ(ft->sent.back()[4], 0x00);
    EXPECT_FALSE(ft->isOpen());
}

// ---------------- ONVIF (fake backend) ----------------
namespace {
struct FakeOnvif : IOnvifPtzBackend {
    OnvifPtzBackendCaps caps;
    std::vector<std::string> log;
    MoveVector lastMove;
    PtzResult connect() override { log.push_back("connect"); return PtzResult::success(); }
    PtzResult disconnect() override { log.push_back("disconnect"); return PtzResult::success(); }
    OnvifPtzBackendCaps capabilities() const override { return caps; }
    PtzResult continuousMove(double p, double t, double z) override {
        lastMove = {p, t, z};
        log.push_back("move");
        return PtzResult::success();
    }
    PtzResult stop(bool, bool) override { log.push_back("stop"); return PtzResult::success(); }
    PtzResult absoluteMove(const PtzPosition&) override { log.push_back("abs"); return PtzResult::success(); }
    PtzResult gotoPreset(int id) override { log.push_back("goto" + std::to_string(id)); return PtzResult::success(); }
    PtzResult setPreset(int id, const std::string&) override { log.push_back("set" + std::to_string(id)); return PtzResult::success(); }
    PtzResult removePreset(int id) override { log.push_back("rm" + std::to_string(id)); return PtzResult::success(); }
    PtzReply<std::vector<PresetInfo>> getPresets() override { return {PtzResult::success(), {{1, "Home"}}}; }
    PtzResult focusMove(double) override { log.push_back("focus"); return PtzResult::success(); }
    PtzResult focusStop() override { log.push_back("focusstop"); return PtzResult::success(); }
    PtzResult setFocusAuto(bool) override { log.push_back("af"); return PtzResult::success(); }
    PtzResult irisStep(int) override { return PtzResult::success(); }
    PtzReply<PtzPosition> getPosition() override { return {PtzResult::success(), {0.1, 0.2, 0.3}}; }
};
}  // namespace

TEST(OnvifProvider, MapsNormalizedSpeedAndCapabilities) {
    auto b = std::make_shared<FakeOnvif>();
    OnvifPtzProvider p(b);
    EXPECT_EQ(p.panLeft().status, PtzStatus::Error);  // not connected
    ASSERT_TRUE(p.connect().ok());
    p.setPanSpeed(0.25);
    p.setTiltSpeed(1.0);
    p.panLeft();
    p.tiltDown();
    EXPECT_DOUBLE_EQ(b->lastMove.pan, -0.25);
    EXPECT_DOUBLE_EQ(b->lastMove.tilt, -1.0);
    EXPECT_DOUBLE_EQ(b->lastMove.zoom, 0.0);
    p.stop();
    EXPECT_EQ(b->log.back(), "stop");
    EXPECT_EQ(p.focusNear().status, PtzStatus::NotSupported);  // backend has no imaging
    EXPECT_EQ(p.focusAuto().status, PtzStatus::NotSupported);
    EXPECT_EQ(p.irisOpen().status, PtzStatus::NotSupported);
    EXPECT_FALSE(p.capabilities().focusAuto);
    EXPECT_TRUE(p.capabilities().presetList);
    auto presets = p.getPresets();
    ASSERT_TRUE(presets.ok());
    EXPECT_EQ(presets.value[0].name, "Home");
    EXPECT_EQ(p.getCameraStatus().value.position->units, PositionUnits::Normalized);
}

TEST(OnvifProvider, NoPtzServiceMeansNotSupported) {
    auto b = std::make_shared<FakeOnvif>();
    b->caps.ptz = false;
    b->caps.presets = false;
    OnvifPtzProvider p(b);
    p.connect();
    EXPECT_EQ(p.panLeft().status, PtzStatus::NotSupported);
    EXPECT_EQ(p.gotoPreset(1).status, PtzStatus::NotSupported);
    EXPECT_EQ(p.getPresets().result.status, PtzStatus::NotSupported);
}

TEST(OnvifProvider, DisconnectStopsFirst) {
    auto b = std::make_shared<FakeOnvif>();
    OnvifPtzProvider p(b);
    p.connect();
    p.zoomIn();
    p.disconnect();
    ASSERT_GE(b->log.size(), 2u);
    EXPECT_EQ(b->log[b->log.size() - 2], "stop");
    EXPECT_EQ(b->log.back(), "disconnect");
}

// ---------------- selection ----------------
TEST(Selection, PicksFirstThatCoversRequired) {
    PTZCapabilities onvifNoFocus, visca, req;
    onvifNoFocus.panTilt = onvifNoFocus.zoom = true;
    visca = onvifNoFocus;
    visca.focusAuto = true;
    req.focusAuto = true;
    EXPECT_EQ(selectProviderIndex({&onvifNoFocus, &visca}, req), 1);
    req.focusAuto = false;
    EXPECT_EQ(selectProviderIndex({&onvifNoFocus, &visca}, req), 0);  // preference order wins
}

TEST(Selection, FallsBackToBestPartialAndDisablesWhenNothingMoves) {
    PTZCapabilities a, b, req;
    a.panTilt = true;
    b.panTilt = b.zoom = true;
    req.zoom = true;
    req.focusAuto = true;
    EXPECT_EQ(selectProviderIndex({&a, &b}, req), 1);
    PTZCapabilities none;
    EXPECT_EQ(selectProviderIndex({&none}, req), -1);
    EXPECT_EQ(selectProviderIndex({}, req), -1);
    EXPECT_EQ(selectProviderIndex({nullptr}, req), -1);
}
