#pragma once
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "onvif/result.hpp"

namespace hb::discovery {

struct Datagram {
    std::string fromIp;
    int fromPort = 0;
    std::string data;
};

// UDP socket bound to one local IPv4 interface, able to send to a multicast group and receive the
// unicast replies. Faked in tests; real implementations: udp_socket_posix.cpp (validated only on
// loopback), udp_socket_win.cpp (NOT validated).
class IUdpMulticastSocket {
public:
    virtual ~IUdpMulticastSocket() = default;
    // localInterfaceIp: IPv4 of the NIC to send from ("" or "0.0.0.0" = OS default).
    virtual hb::onvif::Result<hb::onvif::Unit> open(const std::string& localInterfaceIp) = 0;
    virtual hb::onvif::Result<hb::onvif::Unit> sendTo(const std::string& ip, int port, const std::string& data) = 0;
    // Waits up to `timeout`. Result without value = timed out. Error = socket failure.
    virtual hb::onvif::Result<std::optional<Datagram>> receive(std::chrono::milliseconds timeout) = 0;
    virtual void close() = 0;
};

using UdpSocketFactory = std::function<std::unique_ptr<IUdpMulticastSocket>()>;

// Local IPv4 addresses of up, non-loopback, multicast-capable interfaces.
std::vector<std::string> listLocalIpv4Interfaces();
std::unique_ptr<IUdpMulticastSocket> makeDefaultUdpSocket();

}  // namespace hb::discovery
