#pragma once
#include <memory>
#include <string>
#include <vector>

#include "rtsp/tcp_stream.hpp"
#include "streaming/tls_engine.hpp"

namespace hb::streaming {

// TLS decorator over any hb::rtsp::ITcpStream: itself an ITcpStream, so RtmpClient (or anything else
// written against ITcpStream) gets encryption without knowing. connect() starts the TCP connect;
// pollConnect() then drives the TCP connect AND the TLS handshake and returns Ok only when the
// secure channel is up (WouldBlock while in progress, Error on failure - see lastError() and
// certificateError()). Non-blocking throughout, same contract as the plain stream.
class TlsStream final : public hb::rtsp::ITcpStream {
public:
    TlsStream(std::unique_ptr<hb::rtsp::ITcpStream> inner, std::unique_ptr<ITlsEngine> engine);
    ~TlsStream() override;

    hb::rtsp::IoStatus connect(const std::string& host, std::uint16_t port) override;
    hb::rtsp::IoStatus pollConnect() override;
    hb::rtsp::IoStatus send(const std::uint8_t* data, std::size_t len, std::size_t& sent) override;
    hb::rtsp::IoStatus recv(std::uint8_t* buf, std::size_t cap, std::size_t& got) override;
    bool waitReadable(int timeoutMs) override;
    void close() override;
    std::string lastError() const override { return error_; }

    // True once a failure was a certificate/identity verification failure (never worth retrying).
    bool certificateError() const { return certError_; }

private:
    enum class State { Idle, TcpConnecting, Handshaking, Established, Failed };
    void resetSession();
    hb::rtsp::IoStatus fail(const std::string& msg);
    // Sends as much of out_ as the socket takes. Ok = fully flushed.
    hb::rtsp::IoStatus flushOut();
    // Reads whatever the socket has into in_. Ok = appended >= 1 byte.
    hb::rtsp::IoStatus readMore();

    std::unique_ptr<hb::rtsp::ITcpStream> inner_;
    std::unique_ptr<ITlsEngine> engine_;
    State state_ = State::Idle;
    std::string host_;
    std::string error_;
    bool certError_ = false;
    bool needRead_ = false;
    bool engineDone_ = false;   // engine reported handshake complete; final flight may still be queued
    bool peerClosed_ = false;
    std::vector<std::uint8_t> in_;      // ciphertext received, not yet decrypted
    std::vector<std::uint8_t> out_;     // ciphertext waiting to be sent
    std::vector<std::uint8_t> plain_;   // decrypted bytes not yet handed to the caller
    std::size_t plainOff_ = 0;
    std::size_t outOff_ = 0;
};

}  // namespace hb::streaming
