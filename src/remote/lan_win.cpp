// Windows LAN address enumeration via GetAdaptersAddresses. NOT VALIDATED on Windows (written
// without a Windows compiler): CI is the first compile.
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>

#include <vector>

#include "remote/lan.hpp"

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")

namespace hb::remote {

std::vector<std::string> listLanAddresses() {
    std::vector<std::string> out;
    ULONG size = 16 * 1024;
    std::vector<unsigned char> buf;
    ULONG rc = ERROR_BUFFER_OVERFLOW;
    for (int attempt = 0; attempt < 3 && rc == ERROR_BUFFER_OVERFLOW; ++attempt) {
        buf.assign(size, 0);
        rc = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                  nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &size);
    }
    if (rc != NO_ERROR) return out;
    for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()); a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            if (!u->Address.lpSockaddr || u->Address.lpSockaddr->sa_family != AF_INET) continue;
            const auto* sin = reinterpret_cast<const sockaddr_in*>(u->Address.lpSockaddr);
            char ip[INET_ADDRSTRLEN] = {0};
            if (!InetNtopA(AF_INET, const_cast<in_addr*>(&sin->sin_addr), ip, sizeof ip)) continue;
            if (isPrivateIpv4(ip) && !isLoopbackIpv4(ip)) out.push_back(ip);
        }
    }
    return out;
}

}  // namespace hb::remote
#endif  // _WIN32
