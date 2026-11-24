#include "chat/loopback.hpp"

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
namespace {
using sock_t = SOCKET;
constexpr sock_t kInvalid = INVALID_SOCKET;
void closeSock(sock_t s) { ::closesocket(s); }
bool wsaInit() {
    static const bool ok = [] { WSADATA d; return ::WSAStartup(MAKEWORD(2, 2), &d) == 0; }();
    return ok;
}
}  // namespace
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
namespace {
using sock_t = int;
constexpr sock_t kInvalid = -1;
void closeSock(sock_t s) { ::close(s); }
bool wsaInit() { return true; }
}  // namespace
#endif

namespace hb::chat {
namespace {

// 1 = readable, 0 = timeout, -1 = error
int waitReadable(sock_t s, int ms) {
    fd_set rf;
    FD_ZERO(&rf);
    FD_SET(s, &rf);
    timeval tv;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
#ifdef _WIN32
    const int r = ::select(0, &rf, nullptr, nullptr, &tv);
#else
    const int r = ::select(s + 1, &rf, nullptr, nullptr, &tv);
#endif
    return r < 0 ? -1 : (r == 0 ? 0 : 1);
}

}  // namespace

LoopbackServer::~LoopbackServer() { close(); }

void LoopbackServer::close() {
    if (fd_ != -1) closeSock(static_cast<sock_t>(fd_));
    fd_ = -1;
}

bool LoopbackServer::listen() {
    close();
    if (!wsaInit()) return false;
    sock_t s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s == kInvalid) return false;
    sockaddr_in a;
    std::memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = 0;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // never reachable from other machines
    if (::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || ::listen(s, 4) != 0) {
        closeSock(s);
        return false;
    }
    sockaddr_in bound;
    std::memset(&bound, 0, sizeof bound);
#ifdef _WIN32
    int len = sizeof bound;
#else
    socklen_t len = sizeof bound;
#endif
    if (::getsockname(s, reinterpret_cast<sockaddr*>(&bound), &len) != 0) {
        closeSock(s);
        return false;
    }
    port_ = ntohs(bound.sin_port);
    fd_ = static_cast<std::intptr_t>(s);
    return true;
}

std::string LoopbackServer::waitForRedirect(int timeoutMs, const std::string& responseHtml,
                                            const std::atomic<bool>* cancel) {
    if (fd_ == -1) return {};
    const sock_t ls = static_cast<sock_t>(fd_);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (cancel && cancel->load()) return {};
        const int w = waitReadable(ls, 200);
        if (w < 0) return {};
        if (w == 0) continue;
        sock_t c = ::accept(ls, nullptr, nullptr);
        if (c == kInvalid) continue;
        std::string head;
        char buf[1024];
        const auto connDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (head.size() < 8192 && head.find("\r\n\r\n") == std::string::npos &&
               std::chrono::steady_clock::now() < connDeadline) {
            if (waitReadable(c, 200) <= 0) continue;
            const int n = static_cast<int>(::recv(c, buf, sizeof buf, 0));
            if (n <= 0) break;
            head.append(buf, static_cast<std::size_t>(n));
            if (head.find("\r\n") != std::string::npos && head.find("\r\n\r\n") == std::string::npos &&
                head.size() > 2048)
                break;
        }
        const std::size_t eol = head.find("\r\n");
        const std::string line = head.substr(0, eol);
        const bool hit = line.rfind("GET ", 0) == 0 && (line.find("code=") != std::string::npos ||
                                                       line.find("error=") != std::string::npos);
        std::string resp;
        if (hit) {
            resp = "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: " +
                   std::to_string(responseHtml.size()) + "\r\nConnection: close\r\nCache-Control: no-store\r\n\r\n" + responseHtml;
        } else {
            resp = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        }
#ifdef MSG_NOSIGNAL
        ::send(c, resp.data(), resp.size(), MSG_NOSIGNAL);
#else
        ::send(c, resp.data(), static_cast<int>(resp.size()), 0);
#endif
        closeSock(c);
        if (hit) return head;
    }
    return {};
}

}  // namespace hb::chat
