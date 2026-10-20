// Winsock UDP transport. NOT VALIDATED: written without access to a Windows build.
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include <mutex>
#include <string>

#include "ptz/udp_transport.hpp"

#pragma comment(lib, "ws2_32.lib")

namespace hb::ptz {
namespace {

void ensureWinsock() {
    static std::once_flag once;
    std::call_once(once, [] {
        WSADATA d;
        WSAStartup(MAKEWORD(2, 2), &d);
    });
}

class WinUdp final : public ITransport {
public:
    WinUdp(std::string host, int port) : host_(std::move(host)), port_(port) {}
    ~WinUdp() override { close(); }

    TransportResult open() override {
        ensureWinsock();
        close();
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_DGRAM;
        addrinfo* res = nullptr;
        const std::string svc = std::to_string(port_);
        if (getaddrinfo(host_.c_str(), svc.c_str(), &hints, &res) != 0 || !res)
            return TransportResult::fail("cannot resolve host");
        TransportResult out = TransportResult::fail("connect failed");
        for (addrinfo* a = res; a; a = a->ai_next) {
            SOCKET s = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
            if (s == INVALID_SOCKET) continue;
            if (::connect(s, a->ai_addr, static_cast<int>(a->ai_addrlen)) == 0) {
                sock_ = s;
                out = TransportResult::success();
                break;
            }
            ::closesocket(s);
        }
        freeaddrinfo(res);
        return out;
    }
    void close() override {
        if (sock_ != INVALID_SOCKET) ::closesocket(sock_);
        sock_ = INVALID_SOCKET;
    }
    bool isOpen() const override { return sock_ != INVALID_SOCKET; }

    TransportResult send(const Bytes& d) override {
        if (sock_ == INVALID_SOCKET) return TransportResult::fail("not open");
        const int n = ::send(sock_, reinterpret_cast<const char*>(d.data()), static_cast<int>(d.size()), 0);
        if (n != static_cast<int>(d.size())) return TransportResult::fail("send failed");
        return TransportResult::success();
    }

    TransportResult receive(Bytes& out, Millis timeout) override {
        if (sock_ == INVALID_SOCKET) return TransportResult::fail("not open");
        WSAPOLLFD p{};
        p.fd = sock_;
        p.events = POLLRDNORM;
        const int r = WSAPoll(&p, 1, static_cast<INT>(timeout.count()));
        if (r == 0) return TransportResult::timeout();
        if (r < 0) return TransportResult::fail("poll failed");
        Bytes buf(2048);
        const int n = ::recv(sock_, reinterpret_cast<char*>(buf.data()), static_cast<int>(buf.size()), 0);
        if (n < 0) return TransportResult::fail("recv failed");
        buf.resize(static_cast<std::size_t>(n));
        out = std::move(buf);
        return TransportResult::success();
    }

private:
    std::string host_;
    int port_;
    SOCKET sock_ = INVALID_SOCKET;
};

}  // namespace

std::unique_ptr<ITransport> makeUdpTransport(std::string host, int port) {
    return std::make_unique<WinUdp>(std::move(host), port);
}

}  // namespace hb::ptz
#endif
