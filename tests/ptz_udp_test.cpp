// Loopback test of the real UDP transport (POSIX only; the Winsock variant is not validated).
#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <thread>

#include <gtest/gtest.h>

#include "ptz/udp_transport.hpp"
#include "ptz/visca.hpp"
#include "ptz/visca_provider.hpp"

using namespace hb::ptz;

namespace {
struct Server {
    int fd = -1;
    int port = 0;
    Server() {
        fd = ::socket(AF_INET, SOCK_DGRAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = 0;
        if (::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) { ::close(fd); fd = -1; return; }
        socklen_t l = sizeof a;
        ::getsockname(fd, reinterpret_cast<sockaddr*>(&a), &l);
        port = ntohs(a.sin_port);
        timeval tv{1, 0};
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    }
    ~Server() { if (fd >= 0) ::close(fd); }
    // Reads one datagram and answers with `reply(received)`.
    template <class F>
    bool serveOnce(F reply) {
        unsigned char buf[256];
        sockaddr_in from{};
        socklen_t fl = sizeof from;
        auto n = ::recvfrom(fd, buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &fl);
        if (n <= 0) return false;
        Bytes out = reply(Bytes(buf, buf + n));
        return ::sendto(fd, out.data(), out.size(), 0, reinterpret_cast<sockaddr*>(&from), fl) ==
               static_cast<ssize_t>(out.size());
    }
};
}  // namespace

TEST(Udp, SendReceiveAndTimeoutOnLoopback) {
    Server s;
    ASSERT_GE(s.fd, 0);
    auto t = makeUdpTransport("127.0.0.1", s.port);
    ASSERT_TRUE(t->open().ok);
    Bytes got;
    EXPECT_TRUE(t->receive(got, Millis(20)).timedOut);
    ASSERT_TRUE(t->send({1, 2, 3}).ok);
    ASSERT_TRUE(s.serveOnce([](const Bytes& in) { return Bytes(in.rbegin(), in.rend()); }));
    ASSERT_TRUE(t->receive(got, Millis(500)).ok);
    EXPECT_EQ(got, (Bytes{3, 2, 1}));
    t->close();
    EXPECT_FALSE(t->isOpen());
    EXPECT_FALSE(t->send({1}).ok);
}

TEST(Udp, UnresolvableHostFailsGracefully) {
    auto t = makeUdpTransport("host.invalid", 52381);
    EXPECT_FALSE(t->open().ok);
}

TEST(Udp, ViscaProviderOverRealSocket) {
    Server s;
    ASSERT_GE(s.fd, 0);
    ViscaConfig cfg;
    cfg.sequenceReset = false;
    cfg.ackTimeout = Millis(60);
    ViscaProvider p(makeUdpTransport("127.0.0.1", s.port), cfg);
    ASSERT_TRUE(p.connect().ok());
    std::thread th([&] {
        s.serveOnce([](const Bytes& in) {
            auto f = visca::unwrapIp(in);
            return visca::wrapIp(visca::IpType::Reply, f ? f->sequence : 0, {0x90, 0x41, 0xFF});
        });
    });
    EXPECT_TRUE(p.panLeft().ok());
    th.join();
    EXPECT_EQ(p.zoomIn().status, PtzStatus::Timeout);  // nobody answers
    p.disconnect();
}
#else
#include <gtest/gtest.h>
TEST(Udp, SkippedOnWindows) { GTEST_SKIP() << "POSIX loopback test"; }
#endif
