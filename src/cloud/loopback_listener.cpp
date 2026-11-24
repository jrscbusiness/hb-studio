#include "cloud/loopback_listener.hpp"

#include <chrono>
#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using sock_t = SOCKET;
static const sock_t kBad = INVALID_SOCKET;
static void closeSock(sock_t s) { ::closesocket(s); }
using socklen = int;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using sock_t = int;
static const sock_t kBad = -1;
static void closeSock(sock_t s) { ::close(s); }
using socklen = socklen_t;
#endif

namespace hb::cloud {

namespace {

class Listener : public ILoopbackListener {
public:
    ~Listener() override { close(); }

    int start() override {
#ifdef _WIN32
        WSADATA wsa;
        if (::WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return -1;
        wsa_ = true;
#endif
        srv_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (srv_ == kBad) return -1;
        sockaddr_in a;
        std::memset(&a, 0, sizeof a);
        a.sin_family = AF_INET;
        a.sin_port = 0;
        ::inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);  // loopback ONLY: never reachable from the network
        if (::bind(srv_, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || ::listen(srv_, 1) != 0) {
            close();
            return -1;
        }
        socklen len = sizeof a;
        if (::getsockname(srv_, reinterpret_cast<sockaddr*>(&a), &len) != 0) {
            close();
            return -1;
        }
        return ntohs(a.sin_port);
    }

    bool waitRequestLine(std::string& line, int timeoutMs, const std::function<bool()>& cancelled) override {
        line.clear();
        if (srv_ == kBad) return false;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (std::chrono::steady_clock::now() < deadline) {
            if (cancelled && cancelled()) return false;
            if (!ready(srv_, 200)) continue;
            conn_ = ::accept(srv_, nullptr, nullptr);
            if (conn_ == kBad) return false;
            std::string buf;
            const auto readDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (buf.size() < 8192 && buf.find("\r\n") == std::string::npos && std::chrono::steady_clock::now() < readDeadline) {
                if (!ready(conn_, 200)) continue;
                char tmp[1024];
                const int n = static_cast<int>(::recv(conn_, tmp, sizeof tmp, 0));
                if (n <= 0) break;
                buf.append(tmp, static_cast<std::size_t>(n));
            }
            const auto eol = buf.find("\r\n");
            if (eol == std::string::npos) {  // junk/slow client: drop it and keep waiting for the real redirect
                closeSock(conn_);
                conn_ = kBad;
                continue;
            }
            line = buf.substr(0, eol);
            return true;
        }
        return false;
    }

    void respond(int status, const std::string& html) override {
        if (conn_ == kBad) return;
        const std::string head = std::string("HTTP/1.1 ") + (status == 200 ? "200 OK" : "400 Bad Request") +
                                 "\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: " + std::to_string(html.size()) +
                                 "\r\nConnection: close\r\nCache-Control: no-store\r\n\r\n";
        const std::string out = head + html;
        std::size_t sent = 0;
        while (sent < out.size()) {
            const int n = static_cast<int>(::send(conn_, out.data() + sent, static_cast<int>(out.size() - sent), 0));
            if (n <= 0) break;
            sent += static_cast<std::size_t>(n);
        }
        closeSock(conn_);
        conn_ = kBad;
    }

    void close() override {
        if (conn_ != kBad) { closeSock(conn_); conn_ = kBad; }
        if (srv_ != kBad) { closeSock(srv_); srv_ = kBad; }
#ifdef _WIN32
        if (wsa_) { ::WSACleanup(); wsa_ = false; }
#endif
    }

private:
    static bool ready(sock_t s, int ms) {
        fd_set set;
        FD_ZERO(&set);
        FD_SET(s, &set);
        timeval tv;
        tv.tv_sec = ms / 1000;
        tv.tv_usec = (ms % 1000) * 1000;
        return ::select(static_cast<int>(s) + 1, &set, nullptr, nullptr, &tv) > 0;
    }
    sock_t srv_ = kBad, conn_ = kBad;
#ifdef _WIN32
    bool wsa_ = false;
#endif
};

}  // namespace

std::unique_ptr<ILoopbackListener> makeLoopbackListener() { return std::make_unique<Listener>(); }

}  // namespace hb::cloud
