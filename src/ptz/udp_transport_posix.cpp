#ifndef _WIN32
#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "ptz/udp_transport.hpp"

namespace hb::ptz {
namespace {

class PosixUdp final : public ITransport {
public:
    PosixUdp(std::string host, int port) : host_(std::move(host)), port_(port) {}
    ~PosixUdp() override { close(); }

    TransportResult open() override {
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
            int s = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
            if (s < 0) continue;
            if (::connect(s, a->ai_addr, static_cast<socklen_t>(a->ai_addrlen)) == 0) {
                fd_ = s;
                out = TransportResult::success();
                break;
            }
            ::close(s);
        }
        freeaddrinfo(res);
        return out;
    }
    void close() override {
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
    }
    bool isOpen() const override { return fd_ >= 0; }

    TransportResult send(const Bytes& d) override {
        if (fd_ < 0) return TransportResult::fail("not open");
        const auto n = ::send(fd_, d.data(), d.size(), 0);
        if (n < 0 || static_cast<std::size_t>(n) != d.size()) return TransportResult::fail(std::strerror(errno));
        return TransportResult::success();
    }

    TransportResult receive(Bytes& out, Millis timeout) override {
        if (fd_ < 0) return TransportResult::fail("not open");
        pollfd p{fd_, POLLIN, 0};
        int r;
        do {
            r = ::poll(&p, 1, static_cast<int>(timeout.count()));
        } while (r < 0 && errno == EINTR);
        if (r == 0) return TransportResult::timeout();
        if (r < 0) return TransportResult::fail(std::strerror(errno));
        Bytes buf(2048);
        const auto n = ::recv(fd_, buf.data(), buf.size(), 0);
        if (n < 0) return TransportResult::fail(std::strerror(errno));
        buf.resize(static_cast<std::size_t>(n));
        out = std::move(buf);
        return TransportResult::success();
    }

private:
    std::string host_;
    int port_;
    int fd_ = -1;
};

}  // namespace

std::unique_ptr<ITransport> makeUdpTransport(std::string host, int port) {
    return std::make_unique<PosixUdp>(std::move(host), port);
}

}  // namespace hb::ptz
#endif
