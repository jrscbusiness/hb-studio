// POSIX plain-HTTP client (no TLS). Minimal HTTP/1.1 with "Connection: close".
// Validated only against a local loopback server; NOT validated against a real camera.
#ifndef _WIN32

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cerrno>
#include <cstring>

#include "onvif/http_client.hpp"
#include "onvif/url.hpp"

namespace hb::onvif {
namespace {

using Clock = std::chrono::steady_clock;

struct Fd {
    int fd = -1;
    ~Fd() { if (fd >= 0) ::close(fd); }
};

int remainingMs(Clock::time_point deadline) {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
    return ms < 0 ? 0 : static_cast<int>(std::min<long long>(ms, 1 << 30));
}

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

class PosixHttpClient : public IHttpClient {
public:
    Result<HttpResponse> send(const HttpRequest& req) override {
        const auto deadline = Clock::now() + req.timeout;
        auto parsed = parseUrl(req.url);
        if (!parsed.ok()) return parsed.error();
        const Url& url = parsed.value();
        if (url.scheme != "http") return makeError(ErrorCode::Unsupported, "POSIX client supports only http:// (no TLS)");

        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* res = nullptr;
        if (::getaddrinfo(url.host.c_str(), std::to_string(url.port).c_str(), &hints, &res) != 0 || !res)
            return makeError(ErrorCode::Transport, "cannot resolve host");
        struct Guard { addrinfo* p; ~Guard() { ::freeaddrinfo(p); } } guard{res};

        Fd sock;
        std::string lastErr = "connect failed";
        for (addrinfo* ai = res; ai; ai = ai->ai_next) {
            Fd s;
            s.fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
            if (s.fd < 0) continue;
            ::fcntl(s.fd, F_SETFL, ::fcntl(s.fd, F_GETFL, 0) | O_NONBLOCK);
            int rc = ::connect(s.fd, ai->ai_addr, static_cast<socklen_t>(ai->ai_addrlen));
            if (rc != 0 && errno == EINPROGRESS) {
                pollfd p{s.fd, POLLOUT, 0};
                rc = ::poll(&p, 1, remainingMs(deadline));
                if (rc == 0) return makeError(ErrorCode::Timeout, "connect timeout");
                int err = 0;
                socklen_t len = sizeof err;
                ::getsockopt(s.fd, SOL_SOCKET, SO_ERROR, &err, &len);
                rc = (rc > 0 && err == 0) ? 0 : -1;
                if (rc != 0) lastErr = std::strerror(err ? err : errno);
            }
            if (rc == 0) { sock.fd = s.fd; s.fd = -1; break; }
        }
        if (sock.fd < 0) return makeError(ErrorCode::Transport, lastErr);

        std::string head = req.method + " " + url.path + " HTTP/1.1\r\nHost: " + hostForUrl(url.host);
        if (url.port != 80) head += ":" + std::to_string(url.port);
        head += "\r\nConnection: close\r\nContent-Length: " + std::to_string(req.body.size()) + "\r\n";
        for (const auto& h : req.headers) head += h.first + ": " + h.second + "\r\n";
        head += "\r\n";
        const std::string out = head + req.body;
        for (std::size_t sent = 0; sent < out.size();) {
            pollfd p{sock.fd, POLLOUT, 0};
            if (::poll(&p, 1, remainingMs(deadline)) <= 0) return makeError(ErrorCode::Timeout, "send timeout");
            const ssize_t n = ::send(sock.fd, out.data() + sent, out.size() - sent, MSG_NOSIGNAL);
            if (n < 0) {
                if (errno == EAGAIN || errno == EINTR) continue;
                return makeError(ErrorCode::Transport, std::strerror(errno));
            }
            sent += static_cast<std::size_t>(n);
        }

        std::string raw;
        char buf[8192];
        std::size_t headerEnd = std::string::npos;
        for (;;) {
            pollfd p{sock.fd, POLLIN, 0};
            const int pr = ::poll(&p, 1, remainingMs(deadline));
            if (pr == 0) return makeError(ErrorCode::Timeout, "receive timeout");
            if (pr < 0) {
                if (errno == EINTR) continue;
                return makeError(ErrorCode::Transport, std::strerror(errno));
            }
            const ssize_t n = ::recv(sock.fd, buf, sizeof buf, 0);
            if (n < 0) {
                if (errno == EAGAIN || errno == EINTR) continue;
                return makeError(ErrorCode::Transport, std::strerror(errno));
            }
            if (n == 0) break;
            raw.append(buf, static_cast<std::size_t>(n));
            if (raw.size() > kMaxHttpResponseBytes + 16384) return makeError(ErrorCode::Transport, "response too large");
            if (headerEnd == std::string::npos) headerEnd = raw.find("\r\n\r\n");
            if (headerEnd != std::string::npos && complete(raw, headerEnd)) break;
        }
        return parseResponse(raw);
    }

private:
    static std::string headerValue(const std::string& head, const std::string& name) {
        std::size_t pos = 0;
        while (pos < head.size()) {
            auto eol = head.find("\r\n", pos);
            if (eol == std::string::npos) eol = head.size();
            const std::string line = head.substr(pos, eol - pos);
            const auto colon = line.find(':');
            if (colon != std::string::npos && lower(line.substr(0, colon)) == name) {
                std::size_t b = colon + 1;
                while (b < line.size() && (line[b] == ' ' || line[b] == '\t')) ++b;
                return line.substr(b);
            }
            pos = eol + 2;
        }
        return {};
    }

    static bool complete(const std::string& raw, std::size_t headerEnd) {
        const std::string head = raw.substr(0, headerEnd);
        const std::string bodyPart = raw.substr(headerEnd + 4);
        if (lower(headerValue(head, "transfer-encoding")).find("chunked") != std::string::npos)
            return bodyPart.size() >= 5 && bodyPart.compare(bodyPart.size() - 5, 5, "0\r\n\r\n") == 0;
        const std::string cl = headerValue(head, "content-length");
        if (!cl.empty()) {
            char* end = nullptr;
            const unsigned long long n = std::strtoull(cl.c_str(), &end, 10);
            return bodyPart.size() >= n;
        }
        return false;  // wait for close
    }

    static Result<HttpResponse> parseResponse(const std::string& raw) {
        const auto headerEnd = raw.find("\r\n\r\n");
        if (headerEnd == std::string::npos || raw.compare(0, 5, "HTTP/") != 0)
            return makeError(ErrorCode::Transport, "malformed HTTP response");
        HttpResponse r;
        const auto sp = raw.find(' ');
        if (sp == std::string::npos || sp + 4 > raw.size()) return makeError(ErrorCode::Transport, "malformed status line");
        r.status = std::atoi(raw.c_str() + sp + 1);
        const std::string head = raw.substr(0, headerEnd);
        std::size_t pos = head.find("\r\n");
        while (pos != std::string::npos && pos < head.size()) {
            pos += 2;
            auto eol = head.find("\r\n", pos);
            if (eol == std::string::npos) eol = head.size();
            const std::string line = head.substr(pos, eol - pos);
            const auto colon = line.find(':');
            if (colon != std::string::npos) {
                std::size_t b = colon + 1;
                while (b < line.size() && line[b] == ' ') ++b;
                r.headers.emplace_back(line.substr(0, colon), line.substr(b));
            }
            pos = eol;
        }
        std::string body = raw.substr(headerEnd + 4);
        if (lower(headerValue(head, "transfer-encoding")).find("chunked") != std::string::npos) {
            std::string dec;
            std::size_t p = 0;
            while (p < body.size()) {
                const auto eol = body.find("\r\n", p);
                if (eol == std::string::npos) break;
                const unsigned long n = std::strtoul(body.substr(p, eol - p).c_str(), nullptr, 16);
                if (n == 0) break;
                if (eol + 2 + n > body.size()) { dec.append(body, eol + 2, std::string::npos); break; }
                dec.append(body, eol + 2, n);
                p = eol + 2 + n + 2;
            }
            body = std::move(dec);
        } else {
            const std::string cl = headerValue(head, "content-length");
            if (!cl.empty()) {
                const unsigned long long n = std::strtoull(cl.c_str(), nullptr, 10);
                if (n < body.size()) body.resize(static_cast<std::size_t>(n));
            }
        }
        if (body.size() > kMaxHttpResponseBytes) return makeError(ErrorCode::Transport, "response too large");
        r.body = std::move(body);
        return r;
    }
};

}  // namespace

std::unique_ptr<IHttpClient> makeDefaultHttpClient() { return std::make_unique<PosixHttpClient>(); }

}  // namespace hb::onvif

#endif  // !_WIN32
