#pragma once
#include <chrono>
#include <functional>
#include <string>
#include <vector>

#include "discovery/device.hpp"
#include "discovery/udp_socket.hpp"

namespace hb::discovery {

struct OnvifDiscoveryOptions {
    std::chrono::milliseconds listenTime{3000};  // total time collecting replies
    int probeCount = 2;                          // probes are re-sent evenly spaced (UDP is lossy)
    std::chrono::milliseconds receiveSlice{50};  // max blocking time of one receive()
    // Interfaces (local IPv4) to probe from. Empty = listLocalIpv4Interfaces() (injectable below).
    std::vector<std::string> interfaces;
    // Only report devices whose scopes match ALL these prefixes, e.g. "onvif://www.onvif.org/name/".
    std::vector<std::string> requiredScopes;

    // Injectable dependencies (defaults = real system).
    UdpSocketFactory socketFactory;                                  // default: makeDefaultUdpSocket
    std::function<std::vector<std::string>()> interfaceLister;       // default: listLocalIpv4Interfaces
    std::function<std::chrono::steady_clock::time_point()> now;      // default: steady_clock::now
    std::function<std::string()> messageIdGenerator;                 // default: random "uuid:..."
};

// WS-Discovery (ONVIF) source. One UDP socket per local interface; probes are sent to
// 239.255.255.250:3702 and replies are collected until listenTime elapses.
class OnvifDiscoverySource : public IDiscoverySource {
public:
    explicit OnvifDiscoverySource(OnvifDiscoveryOptions options = {});
    std::string name() const override { return "onvif"; }
    SourceResult discover(const CancelToken& cancel) override;

private:
    OnvifDiscoveryOptions opt_;
};

std::string randomMessageId();

}  // namespace hb::discovery
