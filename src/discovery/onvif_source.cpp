#include "discovery/onvif_source.hpp"

#include <algorithm>
#include <cstdio>
#include <map>
#include <random>

#include "discovery/ws_discovery.hpp"

namespace hb::discovery {

const char* protocolName(Protocol p) {
    switch (p) {
        case Protocol::Onvif: return "onvif";
        case Protocol::Ndi: return "ndi";
        case Protocol::Uvc: return "uvc";
        case Protocol::Rtsp: return "rtsp";
    }
    return "unknown";
}

const char* stateName(DeviceState s) {
    switch (s) {
        case DeviceState::Discovered: return "discovered";
        case DeviceState::RequiresCredentials: return "requires_credentials";
        case DeviceState::Unreachable: return "unreachable";
    }
    return "unknown";
}

std::string randomMessageId() {
    std::random_device rd;
    unsigned char b[16];
    for (auto& x : b) x = static_cast<unsigned char>(rd() & 0xFF);
    b[6] = static_cast<unsigned char>((b[6] & 0x0F) | 0x40);
    b[8] = static_cast<unsigned char>((b[8] & 0x3F) | 0x80);
    char buf[64];
    std::snprintf(buf, sizeof buf, "uuid:%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", b[0], b[1], b[2], b[3],
                  b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    return buf;
}

OnvifDiscoverySource::OnvifDiscoverySource(OnvifDiscoveryOptions options) : opt_(std::move(options)) {
    if (!opt_.socketFactory) opt_.socketFactory = [] { return makeDefaultUdpSocket(); };
    if (!opt_.interfaceLister) opt_.interfaceLister = [] { return listLocalIpv4Interfaces(); };
    if (!opt_.now) opt_.now = [] { return std::chrono::steady_clock::now(); };
    if (!opt_.messageIdGenerator) opt_.messageIdGenerator = randomMessageId;
    if (opt_.probeCount < 1) opt_.probeCount = 1;
}

namespace {

DiscoveredDevice toDevice(const ProbeMatch& pm, const std::string& fromIp) {
    const ScopeInfo scopes = interpretScopes(pm.scopes);
    DiscoveredDevice d;
    std::string uuid = pm.endpointAddress;
    if (uuid.compare(0, 9, "urn:uuid:") == 0) uuid = uuid.substr(9);
    d.ip = ipFromXAddrs(pm.xaddrs);
    if (d.ip.empty()) d.ip = fromIp;
    d.id = "onvif:" + (uuid.empty() ? d.ip : uuid);
    d.name = !scopes.name.empty() ? scopes.name : (!scopes.hardware.empty() ? scopes.hardware : d.ip);
    d.manufacturer = guessManufacturer(scopes.name + " " + scopes.hardware);
    d.protocols = {Protocol::Onvif};
    d.ptz = scopes.ptzHint;
    d.state = DeviceState::RequiresCredentials;  // GetProfiles needs a login on most cameras
    d.xaddrs = pm.xaddrs;
    d.source = "onvif";
    return d;
}

}  // namespace

SourceResult OnvifDiscoverySource::discover(const CancelToken& cancel) {
    SourceResult result;
    std::vector<std::string> ifaces = opt_.interfaces.empty() ? opt_.interfaceLister() : opt_.interfaces;
    if (ifaces.empty()) ifaces.push_back("");  // OS default route

    struct Chan {
        std::unique_ptr<IUdpMulticastSocket> sock;
        std::string iface;
    };
    std::vector<Chan> chans;
    std::string firstError;
    for (const auto& ip : ifaces) {
        auto s = opt_.socketFactory();
        if (!s) continue;
        auto r = s->open(ip);
        if (!r.ok()) {
            if (firstError.empty()) firstError = "open " + ip + ": " + r.error().message;
            continue;
        }
        chans.push_back({std::move(s), ip});
    }
    if (chans.empty()) {
        result.error = firstError.empty() ? "no usable network interface" : firstError;
        return result;
    }

    const std::string messageId = opt_.messageIdGenerator();
    const std::string probe = buildProbe(messageId);
    const auto start = opt_.now();
    const auto deadline = start + opt_.listenTime;
    std::vector<bool> sent(static_cast<std::size_t>(opt_.probeCount), false);
    std::map<std::string, DiscoveredDevice> found;  // by id, preserves one entry per device
    std::vector<std::string> order;
    bool anySendOk = false;

    while (!cancel.cancelled()) {
        const auto t = opt_.now();
        if (t >= deadline) break;
        // Send probe number k at start + k * listenTime / probeCount.
        for (int k = 0; k < opt_.probeCount; ++k) {
            if (sent[static_cast<std::size_t>(k)]) continue;
            if (t - start < opt_.listenTime * k / opt_.probeCount) continue;
            sent[static_cast<std::size_t>(k)] = true;
            for (auto& c : chans) {
                auto r = c.sock->sendTo(kWsDiscoveryGroup, kWsDiscoveryPort, probe);
                if (r.ok()) anySendOk = true;
                else if (firstError.empty()) firstError = "send on " + c.iface + ": " + r.error().message;
            }
        }
        for (auto& c : chans) {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - opt_.now());
            if (remaining.count() <= 0) break;
            auto rx = c.sock->receive(std::min(opt_.receiveSlice, remaining));
            if (!rx.ok()) {
                if (firstError.empty()) firstError = "receive on " + c.iface + ": " + rx.error().message;
                continue;
            }
            if (!rx.value()) continue;
            const Datagram& dg = *rx.value();
            auto pms = parseProbeMatches(dg.data);
            if (!pms.ok()) continue;  // garbage on the port: ignore
            for (const auto& pm : pms.value()) {
                // Ignore replies to somebody else's probe when the device echoes RelatesTo.
                if (!pm.relatesTo.empty() && pm.relatesTo != messageId) continue;
                if (!matchesScopes(pm.scopes, opt_.requiredScopes)) continue;
                DiscoveredDevice d = toDevice(pm, dg.fromIp);
                if (found.emplace(d.id, d).second) order.push_back(d.id);
            }
        }
    }
    for (auto& c : chans) c.sock->close();
    for (const auto& id : order) result.devices.push_back(found[id]);
    if (!anySendOk && result.devices.empty()) result.error = firstError.empty() ? "could not send probe" : firstError;
    return result;
}

}  // namespace hb::discovery
