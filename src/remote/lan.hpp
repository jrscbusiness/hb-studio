#pragma once
// Local-network helpers: which IPv4 addresses are "LAN" (RFC 1918 + link-local), and the list of
// this machine's LAN interface addresses (the only ones the remote server binds).
#include <string>
#include <vector>

namespace hb::remote {

// "192.168.1.5" / "10.x" / "172.16-31.x" / "169.254.x" / "127.x" -> true. Anything malformed -> false.
bool isPrivateIpv4(const std::string& ip);
bool isLoopbackIpv4(const std::string& ip);

// This machine's up, non-loopback IPv4 addresses that are private/link-local. Empty if none (no
// Wi-Fi/Ethernet) or if enumeration fails. POSIX: getifaddrs. Windows: GetAdaptersAddresses
// (lan_win.cpp, not validated on Windows).
std::vector<std::string> listLanAddresses();

}  // namespace hb::remote
