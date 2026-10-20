// POSIX UDP socket for WS-Discovery. Validated only on loopback unicast in a sandbox;
// multicast on a real LAN is NOT validated.
#ifndef _WIN32

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <functional>

#include "discovery/udp_socket.hpp"

namespace hb::discovery {
namespace {

using hb::onvif::ErrorCode;
using hb::onvif::makeError;
using hb::onvif::Result;
using hb::onvif::Unit;

class PosixUdpSocket : public IUdpMulticastSocket {
public:
    ~PosixUdpSocket() override { close(); }

    Result<Unit> open(const std::string& localIp) override {
        close();
        fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (fd_ < 0) return makeError(ErrorCode::Transport, std::strerror(errno));
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_port = 0;  // ephemeral: probe replies come back unicast to this port
        local.sin_addr.s_addr = htonl(INADDR_ANY);
        if (!localIp.empty() && localIp != "0.0.0.0") {
            in_addr ifa{};
            if (::inet_pton(AF_INET, localIp.c_str(), &ifa) != 1) return fail("invalid interface address");
            local.sin_addr = ifa;
            ::setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_IF, &ifa, sizeof ifa);
        }
        const unsigned char ttl = 4;
        ::setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof ttl);
        const unsigned char loop = 1;
        ::setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof loop);
        if (::bind(fd_, reinterpret_cast<sockaddr*>(&local), sizeof local) != 0) return fail(std::strerror(errno));
        return Unit{};
    }

    Result<Unit> sendTo(const std::string& ip, int port, const std::string& data) override {
        if (fd_ < 0) return makeError(ErrorCode::Transport, "socket not open");
        sockaddr_in to{};
        to.sin_family = AF_INET;
        to.sin_port = htons(static_cast<std::uint16_t>(port));
        if (::inet_pton(AF_INET, ip.c_str(), &to.sin_addr) != 1) return makeError(ErrorCode::InvalidArgument, "bad destination IP");
        if (::sendto(fd_, data.data(), data.size(), 0, reinterpret_cast<sockaddr*>(&to), sizeof to) < 0)
            return makeError(ErrorCode::Transport, std::strerror(errno));
        return Unit{};
    }

    Result<std::optional<Datagram>> receive(std::chrono::milliseconds timeout) override {
        if (fd_ < 0) return makeError(ErrorCode::Transport, "socket not open");
        pollfd p{fd_, POLLIN, 0};
        const int rc = ::poll(&p, 1, static_cast<int>(timeout.count()));
        if (rc == 0) return std::optional<Datagram>{};
        if (rc < 0) {
            if (errno == EINTR) return std::optional<Datagram>{};
            return makeError(ErrorCode::Transport, std::strerror(errno));
        }
        char buf[65536];
        sockaddr_in from{};
        socklen_t len = sizeof from;
        const ssize_t n = ::recvfrom(fd_, buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &len);
        if (n < 0) {
            if (errno == EAGAIN || errno == EINTR) return std::optional<Datagram>{};
            return makeError(ErrorCode::Transport, std::strerror(errno));
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
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    }

private:
    Result<Unit> fail(const char* msg) {
        close();
        return makeError(ErrorCode::Transport, msg);
    }
    int fd_ = -1;
};

}  // namespace

std::vector<std::string> listLocalIpv4Interfaces() {
    std::vector<std::string> out;
    ifaddrs* list = nullptr;
    if (::getifaddrs(&list) != 0) return out;
    for (ifaddrs* a = list; a; a = a->ifa_next) {
        if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET) continue;
        if (!(a->ifa_flags & IFF_UP) || (a->ifa_flags & IFF_LOOPBACK) || !(a->ifa_flags & IFF_MULTICAST)) continue;
        char buf[INET_ADDRSTRLEN] = {0};
        ::inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(a->ifa_addr)->sin_addr, buf, sizeof buf);
        out.emplace_back(buf);
    }
    ::freeifaddrs(list);
    return out;
}

std::unique_ptr<IUdpMulticastSocket> makeDefaultUdpSocket() { return std::make_unique<PosixUdpSocket>(); }

}  // namespace hb::discovery

#endif  // !_WIN32
