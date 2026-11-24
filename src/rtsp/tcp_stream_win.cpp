// Winsock TCP stream. NOT VALIDATED: written against the Winsock2 API but never compiled or run
// in this environment (Linux only). Validate on Windows 11 / MSVC 2022 before relying on it.
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
#include <utility>

#include "rtsp/tcp_stream.hpp"

#pragma comment(lib, "ws2_32.lib")

namespace hb::rtsp {

namespace {
void ensureWsa() {
    static std::once_flag once;
    std::call_once(once, [] {
        WSADATA d;
        WSAStartup(MAKEWORD(2, 2), &d);
    });
}

std::string wsaMsg(const char* what, int code) { return std::string(what) + " failed, WSA error " + std::to_string(code); }

class WinTcpStream final : public ITcpStream {
public:
    WinTcpStream() { ensureWsa(); }
    WinTcpStream(SOCKET s, std::string peer) : s_(s), connected_(true), peer_(std::move(peer)) {
        ensureWsa();
        u_long nb = 1;
        ioctlsocket(s_, FIONBIO, &nb);
        BOOL one = TRUE;
        setsockopt(s_, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof one);
    }
    ~WinTcpStream() override { close(); }

    IoStatus connect(const std::string& host, std::uint16_t port) override {
        close();
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* res = nullptr;
        std::string ps = std::to_string(port);
        if (getaddrinfo(host.c_str(), ps.c_str(), &hints, &res) != 0 || !res) {
            err_ = wsaMsg("getaddrinfo", WSAGetLastError());
            return IoStatus::Error;
        }
        s_ = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (s_ == INVALID_SOCKET) {
            err_ = wsaMsg("socket", WSAGetLastError());
            freeaddrinfo(res);
            return IoStatus::Error;
        }
        u_long nb = 1;
        ioctlsocket(s_, FIONBIO, &nb);
        BOOL one = TRUE;
        setsockopt(s_, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof one);
        int c = ::connect(s_, res->ai_addr, static_cast<int>(res->ai_addrlen));
        freeaddrinfo(res);
        if (c == 0) {
            connected_ = true;
            return IoStatus::Ok;
        }
        if (WSAGetLastError() == WSAEWOULDBLOCK) return IoStatus::WouldBlock;
        err_ = wsaMsg("connect", WSAGetLastError());
        close();
        return IoStatus::Error;
    }

    IoStatus pollConnect() override {
        if (connected_) return IoStatus::Ok;
        if (s_ == INVALID_SOCKET) return IoStatus::Error;
        fd_set w, e;
        FD_ZERO(&w);
        FD_ZERO(&e);
        FD_SET(s_, &w);
        FD_SET(s_, &e);
        timeval tv{0, 0};
        int r = select(0, nullptr, &w, &e, &tv);
        if (r == 0) return IoStatus::WouldBlock;
        if (r == SOCKET_ERROR || FD_ISSET(s_, &e)) {
            err_ = wsaMsg("connect", WSAGetLastError());
            return IoStatus::Error;
        }
        connected_ = true;
        return IoStatus::Ok;
    }

    IoStatus send(const std::uint8_t* data, std::size_t len, std::size_t& sent) override {
        sent = 0;
        if (s_ == INVALID_SOCKET || !connected_) return IoStatus::Error;
        if (len == 0) return IoStatus::Ok;
        int n = ::send(s_, reinterpret_cast<const char*>(data), static_cast<int>(len > 0x7fffffff ? 0x7fffffff : len), 0);
        if (n >= 0) {
            sent = static_cast<std::size_t>(n);
            return n > 0 ? IoStatus::Ok : IoStatus::WouldBlock;
        }
        if (WSAGetLastError() == WSAEWOULDBLOCK) return IoStatus::WouldBlock;
        err_ = wsaMsg("send", WSAGetLastError());
        return IoStatus::Error;
    }

    IoStatus recv(std::uint8_t* buf, std::size_t cap, std::size_t& got) override {
        got = 0;
        if (s_ == INVALID_SOCKET || !connected_) return IoStatus::Error;
        int n = ::recv(s_, reinterpret_cast<char*>(buf), static_cast<int>(cap), 0);
        if (n > 0) {
            got = static_cast<std::size_t>(n);
            return IoStatus::Ok;
        }
        if (n == 0) return IoStatus::Closed;
        if (WSAGetLastError() == WSAEWOULDBLOCK) return IoStatus::WouldBlock;
        err_ = wsaMsg("recv", WSAGetLastError());
        return IoStatus::Error;
    }

    bool waitReadable(int timeoutMs) override {
        if (s_ == INVALID_SOCKET) return false;
        WSAPOLLFD p{s_, static_cast<SHORT>(connected_ ? POLLRDNORM : POLLWRNORM), 0};
        return WSAPoll(&p, 1, timeoutMs) > 0;
    }

    void close() override {
        if (s_ != INVALID_SOCKET) closesocket(s_);
        s_ = INVALID_SOCKET;
        connected_ = false;
    }

    std::string lastError() const override { return err_; }
    std::string peerAddress() const override { return peer_; }

private:
    SOCKET s_ = INVALID_SOCKET;
    bool connected_ = false;
    std::string err_;
    std::string peer_;
};

class WinListener final : public TcpListener {
public:
    WinListener(SOCKET s, std::uint16_t port) : s_(s), port_(port) {}
    ~WinListener() override { closesocket(s_); }
    std::uint16_t port() const override { return port_; }
    std::unique_ptr<ITcpStream> accept(int timeoutMs) override {
        WSAPOLLFD p{s_, POLLRDNORM, 0};
        if (WSAPoll(&p, 1, timeoutMs) <= 0) return nullptr;
        sockaddr_in pa{};
        int pl = sizeof pa;
        SOCKET c = ::accept(s_, reinterpret_cast<sockaddr*>(&pa), &pl);
        if (c == INVALID_SOCKET) return nullptr;
        char ip[INET_ADDRSTRLEN] = {0};
        std::string peer;
        if (pl >= static_cast<int>(sizeof(sockaddr_in)) && pa.sin_family == AF_INET &&
            InetNtopA(AF_INET, &pa.sin_addr, ip, sizeof ip))
            peer = ip;
        return std::make_unique<WinTcpStream>(c, std::move(peer));
    }

private:
    SOCKET s_;
    std::uint16_t port_;
};
}  // namespace

std::unique_ptr<ITcpStream> makeSocketTcpStream() { return std::make_unique<WinTcpStream>(); }

std::unique_ptr<TcpListener> makeLoopbackListener(std::uint16_t port) { return makeTcpListener("127.0.0.1", port); }

std::unique_ptr<TcpListener> makeTcpListener(const std::string& ipv4, std::uint16_t port) {
    ensureWsa();
    sockaddr_in a{};
    a.sin_family = AF_INET;
    if (InetPtonA(AF_INET, ipv4.c_str(), &a.sin_addr) != 1) return nullptr;
    a.sin_port = htons(port);
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) return nullptr;
    // No SO_REUSEADDR on Windows: there it lets another process hijack the port.
    if (::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || ::listen(s, 16) != 0) {
        closesocket(s);
        return nullptr;
    }
    int len = sizeof a;
    getsockname(s, reinterpret_cast<sockaddr*>(&a), &len);
    return std::make_unique<WinListener>(s, ntohs(a.sin_port));
}

}  // namespace hb::rtsp

#endif  // _WIN32
