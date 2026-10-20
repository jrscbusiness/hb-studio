#pragma once
// Real UDP transport (connected datagram socket). POSIX implementation in
// udp_transport_posix.cpp, Winsock in udp_transport_win.cpp (NOT validated: no Windows build here).
#include <memory>
#include <string>

#include "ptz/transport.hpp"

namespace hb::ptz {

std::unique_ptr<ITransport> makeUdpTransport(std::string host, int port);

}  // namespace hb::ptz
