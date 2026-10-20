// POSIX (Linux/macOS) TCP stream. Validated by the loopback test on Linux only.
#ifndef _WIN32

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "rtsp/tcp_stream.hpp"

namespace hb::rtsp {

namespace {
#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif

bool setNonBlocking(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    return fl >= 0 && fcntl(fd, F_SETFL, fl | O_NONBLOCK) == 0;
}

class PosixTcpStream final : public ITcpStream {
public:
    PosixTcpStream() = default;
    explicit PosixTcpStream(int fd) : fd_(fd), connected_(true) {
        setNonBlocking(fd_);
        prepare();
    }
    ~PosixTcpStream() override { close(); }

    IoStatus connect(const std::string& host, std::uint16_t port) override {
        close();
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* res = nullptr;
        std::string portStr = std::to_string(port);
        // NOTE: getaddrinfo may block on DNS for literal-less hostnames; cameras are normally
        // addressed by IP. A resolver thread is future work.
        int rc = getaddrinfo(host.c_str(), portStr.c_str(), &hints, &res);
        if (rc != 0 || !res) {
            err_ = std::string("resolve failed: ") + gai_strerror(rc);
            return IoStatus::Error;
        }
        fd_ = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (fd_ < 0 || !setNonBlocking(fd_)) {
            err_ = std::string("socket: ") + std::strerror(errno);
            freeaddrinfo(res);
            close();
            return IoStatus::Error;
        }
        prepare();
        int c = ::connect(fd_, res->ai_addr, static_cast<socklen_t>(res->ai_addrlen));
        freeaddrinfo(res);
        if (c == 0) {
            connected_ = true;
            return IoStatus::Ok;
        }
        if (errno == EINPROGRESS) return IoStatus::WouldBlock;
        err_ = std::string("connect: ") + std::strerror(errno);
        close();
        return IoStatus::Error;
    }

    IoStatus pollConnect() override {
        if (connected_) return IoStatus::Ok;
        if (fd_ < 0) return IoStatus::Error;
        pollfd p{fd_, POLLOUT, 0};
        int r = ::poll(&p, 1, 0);
        if (r == 0) return IoStatus::WouldBlock;
        if (r < 0) {
            if (errno == EINTR) return IoStatus::WouldBlock;
            err_ = std::string("poll: ") + std::strerror(errno);
            return IoStatus::Error;
        }
        int soerr = 0;
        socklen_t len = sizeof soerr;
        getsockopt(fd_, SOL_SOCKET, SO_ERROR, &soerr, &len);
        if (soerr != 0) {
            err_ = std::string("connect: ") + std::strerror(soerr);
            return IoStatus::Error;
        }
        connected_ = true;
        return IoStatus::Ok;
    }

    IoStatus send(const std::uint8_t* data, std::size_t len, std::size_t& sent) override {
        sent = 0;
        if (fd_ < 0 || !connected_) return IoStatus::Error;
        if (len == 0) return IoStatus::Ok;
        ssize_t n = ::send(fd_, data, len, kSendFlags);
        if (n >= 0) {
            sent = static_cast<std::size_t>(n);
            return n > 0 ? IoStatus::Ok : IoStatus::WouldBlock;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return IoStatus::WouldBlock;
        err_ = std::string("send: ") + std::strerror(errno);
        return IoStatus::Error;
    }

    IoStatus recv(std::uint8_t* buf, std::size_t cap, std::size_t& got) override {
        got = 0;
        if (fd_ < 0 || !connected_) return IoStatus::Error;
        ssize_t n = ::recv(fd_, buf, cap, 0);
        if (n > 0) {
            got = static_cast<std::size_t>(n);
            return IoStatus::Ok;
        }
        if (n == 0) return IoStatus::Closed;
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return IoStatus::WouldBlock;
        err_ = std::string("recv: ") + std::strerror(errno);
        return IoStatus::Error;
    }

    bool waitReadable(int timeoutMs) override {
        if (fd_ < 0) return false;
        pollfd p{fd_, static_cast<short>(connected_ ? POLLIN : POLLOUT), 0};
        int r = ::poll(&p, 1, timeoutMs);
        return r > 0;
    }

    void close() override {
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
        connected_ = false;
    }

    std::string lastError() const override { return err_; }

private:
    void prepare() {
        int one = 1;
        setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
#ifdef SO_NOSIGPIPE
        setsockopt(fd_, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    }

    int fd_ = -1;
    bool connected_ = false;
    std::string err_;
};

class PosixListener final : public TcpListener {
public:
    PosixListener(int fd, std::uint16_t port) : fd_(fd), port_(port) {}
    ~PosixListener() override { ::close(fd_); }
    std::uint16_t port() const override { return port_; }
    std::unique_ptr<ITcpStream> accept(int timeoutMs) override {
        pollfd p{fd_, POLLIN, 0};
        if (::poll(&p, 1, timeoutMs) <= 0) return nullptr;
        int c = ::accept(fd_, nullptr, nullptr);
        if (c < 0) return nullptr;
        return std::make_unique<PosixTcpStream>(c);
    }

private:
    int fd_;
    std::uint16_t port_;
};
}  // namespace

std::unique_ptr<ITcpStream> makeSocketTcpStream() { return std::make_unique<PosixTcpStream>(); }

std::unique_ptr<TcpListener> makeLoopbackListener(std::uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return nullptr;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(port);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || ::listen(fd, 8) != 0) {
        ::close(fd);
        return nullptr;
    }
    socklen_t len = sizeof a;
    getsockname(fd, reinterpret_cast<sockaddr*>(&a), &len);
    return std::make_unique<PosixListener>(fd, ntohs(a.sin_port));
}

}  // namespace hb::rtsp

#endif  // !_WIN32
