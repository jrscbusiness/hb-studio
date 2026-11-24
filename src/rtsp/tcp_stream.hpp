#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace hb::rtsp {

enum class IoStatus {
    Ok,          // operation done (recv: at least 1 byte; send: at least 1 byte accepted)
    WouldBlock,  // nothing to do right now (non-blocking); retry later
    Closed,      // orderly close by the peer
    Error,       // socket error; see lastError()
};

// Non-blocking TCP client stream. Everything above this interface (session, receiver, input) is
// pure logic and is tested with an in-memory fake; the POSIX and Winsock implementations are the
// only platform specific code.
class ITcpStream {
public:
    virtual ~ITcpStream() = default;
    // Starts connecting. Ok = connected already, WouldBlock = in progress (poll pollConnect()).
    virtual IoStatus connect(const std::string& host, std::uint16_t port) = 0;
    virtual IoStatus pollConnect() = 0;  // Ok when connected, WouldBlock while pending
    virtual IoStatus send(const std::uint8_t* data, std::size_t len, std::size_t& sent) = 0;
    virtual IoStatus recv(std::uint8_t* buf, std::size_t cap, std::size_t& got) = 0;
    // Waits up to timeoutMs for incoming data / connect completion; returns early on activity.
    virtual bool waitReadable(int timeoutMs) = 0;
    virtual void close() = 0;
    virtual std::string lastError() const = 0;
    // Remote IP of an ACCEPTED stream (dotted IPv4, or "" if unknown / not an accepted stream).
    // Non-pure with a default so existing fakes/implementations are unaffected.
    virtual std::string peerAddress() const { return {}; }
};

using TcpStreamFactory = std::function<std::unique_ptr<ITcpStream>()>;

// Real socket implementation (POSIX: tcp_stream_posix.cpp, Windows: tcp_stream_win.cpp).
std::unique_ptr<ITcpStream> makeSocketTcpStream();

// Minimal listening socket, used by loopback tests and the future RTSP simulator tool.
class TcpListener {
public:
    virtual ~TcpListener() = default;
    virtual std::uint16_t port() const = 0;
    // Blocks up to timeoutMs; nullptr on timeout. The accepted stream is already connected.
    virtual std::unique_ptr<ITcpStream> accept(int timeoutMs) = 0;
};
// Listens on 127.0.0.1:port (0 = ephemeral). nullptr on failure.
std::unique_ptr<TcpListener> makeLoopbackListener(std::uint16_t port = 0);
// Listens on one specific local IPv4 address (dotted literal, e.g. "192.168.0.10"; "127.0.0.1" is
// the loopback case). nullptr on failure (bad literal, address not local, port in use...).
// Added for the remote-control server, which binds the LAN addresses only (never 0.0.0.0).
std::unique_ptr<TcpListener> makeTcpListener(const std::string& ipv4, std::uint16_t port = 0);

}  // namespace hb::rtsp
