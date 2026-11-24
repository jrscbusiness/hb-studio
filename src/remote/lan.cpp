#include "remote/lan.hpp"

#include <cstdio>

#ifndef _WIN32
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#endif

namespace hb::remote {

namespace {
bool parseQuad(const std::string& ip, int q[4]) {
    if (ip.size() < 7 || ip.size() > 15) return false;
    char tail = 0;
    if (std::sscanf(ip.c_str(), "%d.%d.%d.%d%c", &q[0], &q[1], &q[2], &q[3], &tail) != 4) return false;
    for (int i = 0; i < 4; ++i)
        if (q[i] < 0 || q[i] > 255) return false;
    // sscanf accepts "+1" or " 1"; reject anything that is not plain digits and dots.
    for (char c : ip)
        if (!((c >= '0' && c <= '9') || c == '.')) return false;
    return true;
}
}  // namespace

bool isLoopbackIpv4(const std::string& ip) {
    int q[4];
    return parseQuad(ip, q) && q[0] == 127;
}

bool isPrivateIpv4(const std::string& ip) {
    int q[4];
    if (!parseQuad(ip, q)) return false;
    if (q[0] == 10 || q[0] == 127) return true;
    if (q[0] == 172 && q[1] >= 16 && q[1] <= 31) return true;
    if (q[0] == 192 && q[1] == 168) return true;
    if (q[0] == 169 && q[1] == 254) return true;
    return false;
}

#ifndef _WIN32
std::vector<std::string> listLanAddresses() {
    std::vector<std::string> out;
    ifaddrs* ifa = nullptr;
    if (getifaddrs(&ifa) != 0) return out;
    for (ifaddrs* p = ifa; p; p = p->ifa_next) {
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
        if (!(p->ifa_flags & IFF_UP) || (p->ifa_flags & IFF_LOOPBACK)) continue;
        char buf[INET_ADDRSTRLEN] = {0};
        const auto* sin = reinterpret_cast<const sockaddr_in*>(p->ifa_addr);
        if (!inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof buf)) continue;
        const std::string ip = buf;
        if (isPrivateIpv4(ip) && !isLoopbackIpv4(ip)) out.push_back(ip);
    }
    freeifaddrs(ifa);
    return out;
}
#endif

}  // namespace hb::remote
