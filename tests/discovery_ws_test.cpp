#include <gtest/gtest.h>

#include <deque>
#include <map>

#include <pugixml.hpp>

#include "discovery/onvif_source.hpp"
#include "discovery/ws_discovery.hpp"

using namespace hb::discovery;

namespace {

// Hikvision-like ProbeMatches reply (reconstructed, not captured).
std::string probeMatches(const std::string& relatesTo, const std::string& uuid, const std::string& ip, const std::string& scopes) {
    return "<?xml version=\"1.0\" encoding=\"UTF-8\"?><SOAP-ENV:Envelope xmlns:SOAP-ENV=\"http://www.w3.org/2003/05/soap-envelope\" "
           "xmlns:wsa=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\" xmlns:d=\"http://schemas.xmlsoap.org/ws/2005/04/discovery\" "
           "xmlns:dn=\"http://www.onvif.org/ver10/network/wsdl\"><SOAP-ENV:Header><wsa:MessageID>uuid:" + uuid +
           "</wsa:MessageID><wsa:RelatesTo>" + relatesTo +
           "</wsa:RelatesTo><wsa:To>http://schemas.xmlsoap.org/ws/2004/08/addressing/role/anonymous</wsa:To>"
           "<wsa:Action>http://schemas.xmlsoap.org/ws/2005/04/discovery/ProbeMatches</wsa:Action></SOAP-ENV:Header><SOAP-ENV:Body>"
           "<d:ProbeMatches><d:ProbeMatch><wsa:EndpointReference><wsa:Address>urn:uuid:" + uuid +
           "</wsa:Address></wsa:EndpointReference><d:Types>dn:NetworkVideoTransmitter tds:Device</d:Types><d:Scopes>" + scopes +
           "</d:Scopes><d:XAddrs>http://" + ip + "/onvif/device_service</d:XAddrs><d:MetadataVersion>10</d:MetadataVersion></d:ProbeMatch>"
           "</d:ProbeMatches></SOAP-ENV:Body></SOAP-ENV:Envelope>";
}

const char* kHikScopes =
    "onvif://www.onvif.org/type/video_encoder onvif://www.onvif.org/type/ptz onvif://www.onvif.org/location/country/china "
    "onvif://www.onvif.org/name/Hikvision%20DS-2DE4425 onvif://www.onvif.org/hardware/DS-2DE4425IW-DE onvif://www.onvif.org/Profile/Streaming "
    "onvif://www.onvif.org/Profile/T";

}  // namespace

TEST(WsDiscovery, ProbeIsWellFormedAndTargetsNvt) {
    const auto xml = buildProbe("uuid:abc-123");
    pugi::xml_document d;
    ASSERT_TRUE(d.load_string(xml.c_str()));
    auto env = d.child("e:Envelope");
    EXPECT_STREQ(env.child("e:Header").child("w:MessageID").text().get(), "uuid:abc-123");
    EXPECT_STREQ(env.child("e:Header").child("w:Action").text().get(), "http://schemas.xmlsoap.org/ws/2005/04/discovery/Probe");
    EXPECT_STREQ(env.child("e:Body").child("d:Probe").child("d:Types").text().get(), "dn:NetworkVideoTransmitter");
    EXPECT_NE(xml.find("xmlns:dn=\"http://www.onvif.org/ver10/network/wsdl\""), std::string::npos);
    EXPECT_EQ(std::string(kWsDiscoveryGroup), "239.255.255.250");
    EXPECT_EQ(kWsDiscoveryPort, 3702);
}

TEST(WsDiscovery, ParsesProbeMatches) {
    auto r = parseProbeMatches(probeMatches("uuid:p1", "2419d68a-2dd2-21b2-a205-c0a8016400", "192.168.1.64", kHikScopes));
    ASSERT_TRUE(r.ok());
    ASSERT_EQ(r.value().size(), 1u);
    const auto& m = r.value()[0];
    EXPECT_EQ(m.endpointAddress, "urn:uuid:2419d68a-2dd2-21b2-a205-c0a8016400");
    EXPECT_EQ(m.relatesTo, "uuid:p1");
    EXPECT_EQ(m.xaddrs, std::vector<std::string>{"http://192.168.1.64/onvif/device_service"});
    EXPECT_EQ(m.types.size(), 2u);
    EXPECT_EQ(m.scopes.size(), 7u);
    EXPECT_EQ(m.metadataVersion, 10);
}

TEST(WsDiscovery, MultipleXAddrsAndMultipleMatches) {
    const std::string x =
        "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\" xmlns:d=\"http://schemas.xmlsoap.org/ws/2005/04/discovery\" "
        "xmlns:a=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\"><s:Body><d:ProbeMatches>"
        "<d:ProbeMatch><a:EndpointReference><a:Address>urn:uuid:A</a:Address></a:EndpointReference>"
        "<d:XAddrs>http://169.254.7.7/onvif/device_service http://192.168.5.20:8000/onvif/device_service</d:XAddrs></d:ProbeMatch>"
        "<d:ProbeMatch><a:EndpointReference><a:Address>urn:uuid:B</a:Address></a:EndpointReference><d:XAddrs>http://[fe80::1]/x</d:XAddrs></d:ProbeMatch>"
        "<d:ProbeMatch/></d:ProbeMatches></s:Body></s:Envelope>";
    auto r = parseProbeMatches(x);
    ASSERT_TRUE(r.ok());
    ASSERT_EQ(r.value().size(), 2u);  // empty match dropped
    EXPECT_EQ(ipFromXAddrs(r.value()[0].xaddrs), "192.168.5.20");  // link-local deprioritised
    EXPECT_EQ(ipFromXAddrs(r.value()[1].xaddrs), "fe80::1");
    EXPECT_EQ(ipFromXAddrs({}), "");
}

TEST(WsDiscovery, RejectsGarbageAndOtherMessages) {
    EXPECT_FALSE(parseProbeMatches("").ok());
    EXPECT_FALSE(parseProbeMatches("\x01\x02\x03").ok());
    EXPECT_FALSE(parseProbeMatches("<a><b></a>").ok());
    EXPECT_FALSE(parseProbeMatches(buildProbe("uuid:x")).ok());  // a Probe is not a ProbeMatches
    EXPECT_FALSE(parseProbeMatches(std::string(100000, '<')).ok());
}

TEST(WsDiscovery, ScopesInterpretation) {
    std::vector<std::string> scopes;
    for (const char* s : {"onvif://www.onvif.org/name/Hikvision%20DS-2DE4425", "onvif://www.onvif.org/hardware/DS-2DE4425IW-DE",
                          "onvif://www.onvif.org/location/country/china", "onvif://www.onvif.org/Profile/Streaming",
                          "onvif://www.onvif.org/Profile/T", "onvif://www.onvif.org/type/ptz", "http://other/scope"})
        scopes.push_back(s);
    const auto info = interpretScopes(scopes);
    EXPECT_EQ(info.name, "Hikvision DS-2DE4425");
    EXPECT_EQ(info.hardware, "DS-2DE4425IW-DE");
    EXPECT_EQ(info.location, "country/china");
    EXPECT_EQ(info.profiles, (std::vector<std::string>{"Streaming", "T"}));
    EXPECT_TRUE(info.ptzHint);
    EXPECT_EQ(percentDecode("a%20b%2"), "a b%2");  // truncated escape kept verbatim
    EXPECT_EQ(percentDecode("100%"), "100%");
    EXPECT_FALSE(interpretScopes({}).ptzHint);
}

TEST(WsDiscovery, ScopeFilter) {
    const std::vector<std::string> scopes = {"onvif://www.onvif.org/name/Cam1", "onvif://www.onvif.org/hardware/DS-2CD2043"};
    EXPECT_TRUE(matchesScopes(scopes, {}));
    EXPECT_TRUE(matchesScopes(scopes, {"onvif://www.onvif.org/name/"}));
    EXPECT_TRUE(matchesScopes(scopes, {"ONVIF://www.onvif.org/hardware/DS-2"}));
    EXPECT_TRUE(matchesScopes(scopes, {"onvif://www.onvif.org/name/", "onvif://www.onvif.org/hardware/"}));
    EXPECT_FALSE(matchesScopes(scopes, {"onvif://www.onvif.org/name/", "onvif://www.onvif.org/type/ptz"}));
    EXPECT_FALSE(matchesScopes({}, {"x"}));
}

TEST(WsDiscovery, ManufacturerHeuristic) {
    EXPECT_EQ(guessManufacturer("Hikvision DS-2DE4425"), "Hikvision");
    EXPECT_EQ(guessManufacturer("DS-2CD2043"), "Hikvision");
    EXPECT_EQ(guessManufacturer("DH-SD49225"), "Dahua");
    EXPECT_EQ(guessManufacturer("AXIS P1448-LE"), "Axis");
    EXPECT_EQ(guessManufacturer("Mystery Cam"), "");
}

// ---------------- OnvifDiscoverySource with a fake socket and a fake clock ----------------

namespace {

struct FakeNet {
    std::chrono::steady_clock::time_point now{};
    std::vector<std::string> probesSent;
};

class FakeUdp : public IUdpMulticastSocket {
public:
    FakeUdp(FakeNet& net, std::deque<Datagram> inbox, bool failOpen = false) : net_(net), inbox_(std::move(inbox)), failOpen_(failOpen) {}
    hb::onvif::Result<hb::onvif::Unit> open(const std::string& ip) override {
        iface = ip;
        if (failOpen_) return hb::onvif::makeError(hb::onvif::ErrorCode::Transport, "bind failed");
        return hb::onvif::Unit{};
    }
    hb::onvif::Result<hb::onvif::Unit> sendTo(const std::string& ip, int port, const std::string& data) override {
        EXPECT_EQ(ip, "239.255.255.250");
        EXPECT_EQ(port, 3702);
        net_.probesSent.push_back(data);
        return hb::onvif::Unit{};
    }
    hb::onvif::Result<std::optional<Datagram>> receive(std::chrono::milliseconds timeout) override {
        if (!inbox_.empty()) {
            auto d = std::move(inbox_.front());
            inbox_.pop_front();
            return std::optional<Datagram>{std::move(d)};
        }
        net_.now += timeout;  // simulated wait
        return std::optional<Datagram>{};
    }
    void close() override { closed = true; }
    std::string iface;
    bool closed = false;

private:
    FakeNet& net_;
    std::deque<Datagram> inbox_;
    bool failOpen_;
};

OnvifDiscoveryOptions fakeOptions(FakeNet& net, std::vector<std::unique_ptr<FakeUdp>>& pool, std::vector<std::string> ifaces) {
    OnvifDiscoveryOptions o;
    o.interfaces = std::move(ifaces);
    o.now = [&net] { return net.now; };
    o.messageIdGenerator = [] { return "uuid:test-probe"; };
    o.socketFactory = [&pool]() -> std::unique_ptr<IUdpMulticastSocket> {
        if (pool.empty()) return nullptr;
        auto s = std::move(pool.front());
        pool.erase(pool.begin());
        return s;
    };
    return o;
}

}  // namespace

TEST(OnvifSource, CollectsDevicesFromReplies) {
    FakeNet net;
    std::vector<std::unique_ptr<FakeUdp>> pool;
    std::deque<Datagram> in;
    in.push_back({"192.168.1.64", 3702, probeMatches("uuid:test-probe", "AAA", "192.168.1.64", kHikScopes)});
    in.push_back({"192.168.1.9", 3702, "garbage that must be ignored"});
    in.push_back({"192.168.1.108", 3702, probeMatches("uuid:test-probe", "BBB", "192.168.1.108", "onvif://www.onvif.org/name/Dahua%20SD49")});
    in.push_back({"192.168.1.64", 3702, probeMatches("uuid:test-probe", "AAA", "192.168.1.64", kHikScopes)});  // duplicate
    in.push_back({"192.168.1.77", 3702, probeMatches("uuid:someone-else", "CCC", "192.168.1.77", "onvif://www.onvif.org/name/Foreign")});
    pool.push_back(std::make_unique<FakeUdp>(net, std::move(in)));

    OnvifDiscoverySource src(fakeOptions(net, pool, {"192.168.1.5"}));
    auto res = src.discover(hb::discovery::CancelToken{});
    ASSERT_TRUE(res.ok()) << res.error;
    ASSERT_EQ(res.devices.size(), 2u);
    EXPECT_EQ(res.devices[0].id, "onvif:AAA");
    EXPECT_EQ(res.devices[0].ip, "192.168.1.64");
    EXPECT_EQ(res.devices[0].name, "Hikvision DS-2DE4425");
    EXPECT_EQ(res.devices[0].manufacturer, "Hikvision");
    EXPECT_TRUE(res.devices[0].ptz);
    EXPECT_EQ(res.devices[0].state, DeviceState::RequiresCredentials);
    EXPECT_EQ(res.devices[0].protocols, std::vector<Protocol>{Protocol::Onvif});
    EXPECT_EQ(res.devices[1].manufacturer, "Dahua");
    EXPECT_FALSE(res.devices[1].ptz);
    // Two probes were sent (probeCount default 2), both with our MessageID.
    ASSERT_EQ(net.probesSent.size(), 2u);
    EXPECT_NE(net.probesSent[0].find("uuid:test-probe"), std::string::npos);
}

TEST(OnvifSource, AppliesScopeFilter) {
    FakeNet net;
    std::vector<std::unique_ptr<FakeUdp>> pool;
    std::deque<Datagram> in;
    in.push_back({"1.1.1.1", 3702, probeMatches("uuid:test-probe", "AAA", "192.168.1.64", kHikScopes)});
    in.push_back({"1.1.1.2", 3702, probeMatches("uuid:test-probe", "BBB", "192.168.1.65", "onvif://www.onvif.org/name/Other")});
    pool.push_back(std::make_unique<FakeUdp>(net, std::move(in)));
    auto opt = fakeOptions(net, pool, {"192.168.1.5"});
    opt.requiredScopes = {"onvif://www.onvif.org/hardware/DS-"};
    OnvifDiscoverySource src(opt);
    auto res = src.discover(hb::discovery::CancelToken{});
    ASSERT_EQ(res.devices.size(), 1u);
    EXPECT_EQ(res.devices[0].id, "onvif:AAA");
}

TEST(OnvifSource, OneInterfaceFailingDoesNotStopTheOthers) {
    FakeNet net;
    std::vector<std::unique_ptr<FakeUdp>> pool;
    pool.push_back(std::make_unique<FakeUdp>(net, std::deque<Datagram>{}, /*failOpen=*/true));
    std::deque<Datagram> in;
    in.push_back({"10.0.0.7", 3702, probeMatches("uuid:test-probe", "ZZZ", "10.0.0.7", "onvif://www.onvif.org/name/N")});
    pool.push_back(std::make_unique<FakeUdp>(net, std::move(in)));
    OnvifDiscoverySource src(fakeOptions(net, pool, {"169.254.0.1", "10.0.0.2"}));
    auto res = src.discover(hb::discovery::CancelToken{});
    EXPECT_TRUE(res.ok());
    EXPECT_EQ(res.devices.size(), 1u);
}

TEST(OnvifSource, AllInterfacesFailingIsAnError) {
    FakeNet net;
    std::vector<std::unique_ptr<FakeUdp>> pool;
    pool.push_back(std::make_unique<FakeUdp>(net, std::deque<Datagram>{}, true));
    OnvifDiscoverySource src(fakeOptions(net, pool, {"10.0.0.2"}));
    auto res = src.discover(hb::discovery::CancelToken{});
    EXPECT_FALSE(res.ok());
    EXPECT_NE(res.error.find("bind failed"), std::string::npos);
}

TEST(OnvifSource, HonoursCancellationAndListenTimeWithFakeClock) {
    FakeNet net;
    std::vector<std::unique_ptr<FakeUdp>> pool;
    pool.push_back(std::make_unique<FakeUdp>(net, std::deque<Datagram>{}));
    auto opt = fakeOptions(net, pool, {"10.0.0.2"});
    opt.listenTime = std::chrono::seconds(3);  // simulated: returns instantly
    OnvifDiscoverySource src(opt);
    auto res = src.discover(hb::discovery::CancelToken{});
    EXPECT_TRUE(res.ok());
    EXPECT_TRUE(res.devices.empty());
    EXPECT_GE(net.now - std::chrono::steady_clock::time_point{}, std::chrono::seconds(3));

    FakeNet net2;
    std::vector<std::unique_ptr<FakeUdp>> pool2;
    pool2.push_back(std::make_unique<FakeUdp>(net2, std::deque<Datagram>{}));
    OnvifDiscoverySource src2(fakeOptions(net2, pool2, {"10.0.0.2"}));
    hb::discovery::CancelToken cancelled;
    cancelled.cancel();
    EXPECT_TRUE(src2.discover(cancelled).devices.empty());
    EXPECT_TRUE(net2.probesSent.empty());
}
