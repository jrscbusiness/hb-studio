// Winsock UDP socket for WS-Discovery. NOT VALIDATED: written without a Windows compiler/network.
// Build and test on Windows 11 / MSVC 2022 before relying on it (see docs/ONVIF.md).
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

#include <functional>
#include <mutex>

#include "discovery/udp_socket.hpp"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")

namespace hb::discovery {
namespace {

using hb::onvif::ErrorCode;
using hb::onvif::makeError;
using hb::onvif::Result;
using hb::onvif::Unit;

bool ensureWinsock() {
    static std::once_flag once;
    static bool ok = false;
    std::call_once(once, [] {
        WSADATA d;
        ok = ::WSAStartup(MAKEWORD(2, 2), &d) == 0;
    });
    return ok;
}

std::string wsaMsg(const char* what) { return std::string(what) + " failed (WSA error " + std::to_string(::WSAGetLastError()) + ")"; }

class WinUdpSocket : public IUdpMulticastSocket {
public:
    ~WinUdpSocket() override { close(); }

    Result<Unit> open(const std::string& localIp) override {
        close();
        if (!ensureWinsock()) return makeError(ErrorCode::Transport, "WSAStartup failed");
        s_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s_ == INVALID_SOCKET) return makeError(ErrorCode::Transport, wsaMsg("socket"));
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_port = 0;
        local.sin_addr.s_addr = htonl(INADDR_ANY);
        if (!localIp.empty() && localIp != "0.0.0.0") {
            in_addr ifa{};
            if (::inet_pton(AF_INET, localIp.c_str(), &ifa) != 1) return fail("invalid interface address");
            local.sin_addr = ifa;
            ::setsockopt(s_, IPPROTO_IP, IP_MULTICAST_IF, reinterpret_cast<const char*>(&ifa), sizeof ifa);
        }
        const DWORD ttl = 4;
        ::setsockopt(s_, IPPROTO_IP, IP_MULTICAST_TTL, reinterpret_cast<const char*>(&ttl), sizeof ttl);
        if (::bind(s_, reinterpret_cast<sockaddr*>(&local), sizeof local) != 0) return fail(wsaMsg("bind").c_str());
        return Unit{};
    }

    Result<Unit> sendTo(const std::string& ip, int port, const std::string& data) override {
        if (s_ == INVALID_SOCKET) return makeError(ErrorCode::Transport, "socket not open");
        sockaddr_in to{};
        to.sin_family = AF_INET;
        to.sin_port = htons(static_cast<u_short>(port));
        if (::inet_pton(AF_INET, ip.c_str(), &to.sin_addr) != 1) return makeError(ErrorCode::InvalidArgument, "bad destination IP");
        if (::sendto(s_, data.data(), static_cast<int>(data.size()), 0, reinterpret_cast<sockaddr*>(&to), sizeof to) == SOCKET_ERROR)
            return makeError(ErrorCode::Transport, wsaMsg("sendto"));
        return Unit{};
    }

    Result<std::optional<Datagram>> receive(std::chrono::milliseconds timeout) override {
        if (s_ == INVALID_SOCKET) return makeError(ErrorCode::Transport, "socket not open");
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(s_, &rd);
        timeval tv{static_cast<long>(timeout.count() / 1000), static_cast<long>(timeout.count() % 1000 * 1000)};
        const int rc = ::select(0, &rd, nullptr, nullptr, &tv);
        if (rc == 0) return std::optional<Datagram>{};
        if (rc == SOCKET_ERROR) return makeError(ErrorCode::Transport, wsaMsg("select"));
        static thread_local char buf[65536];
        sockaddr_in from{};
        int len = sizeof from;
        const int n = ::recvfrom(s_, buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &len);
        if (n == SOCKET_ERROR) {
            // WSAECONNRESET is delivered after ICMP port-unreachable on UDP; ignore it.
            if (::WSAGetLastError() == WSAECONNRESET) return std::optional<Datagram>{};
            return makeError(ErrorCode::Transport, wsaMsg("recvfrom"));
        }
        char ipbuf[INET_ADDRSTRLEN] = {0};
        ::inet_ntop(AF_INET, &from.sin_addr, ipbuf, sizeof ipbuf);
        Datagram d;
        d.fromIp = ipbuf;
        d.fromPort = ntohs(from.sin_port);
        d.data.assign(buf, static_cast<std::size_t>(n));
        return std::optional<Datagram>{std::move(d)};
    }

    void close() override {
        if (s_ != INVALID_SOCKET) { ::closesocket(s_); s_ = INVALID_SOCKET; }
    }

private:
    Result<Unit> fail(const char* msg) {
        const std::string m = msg;
        close();
        return makeError(ErrorCode::Transport, m);
    }
    SOCKET s_ = INVALID_SOCKET;
};

}  // namespace

std::vector<std::string> listLocalIpv4Interfaces() {
    std::vector<std::string> out;
    ULONG size = 16 * 1024;
    std::vector<char> buf(size);
    ULONG rc = ::GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr,
                                      reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &size);
    if (rc == ERROR_BUFFER_OVERFLOW) {
        buf.resize(size);
        rc = ::GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr,
                                    reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &size);
    }
    if (rc != NO_ERROR) return out;
    for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()); a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK || (a->Flags & IP_ADAPTER_NO_MULTICAST)) continue;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            if (!u->Address.lpSockaddr || u->Address.lpSockaddr->sa_family != AF_INET) continue;
            char ipbuf[INET_ADDRSTRLEN] = {0};
            ::inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr)->sin_addr, ipbuf, sizeof ipbuf);
            out.emplace_back(ipbuf);
        }
    }
    return out;
}

std::unique_ptr<IUdpMulticastSocket> makeDefaultUdpSocket() { return std::make_unique<WinUdpSocket>(); }

}  // namespace hb::discovery

#endif  // _WIN32
