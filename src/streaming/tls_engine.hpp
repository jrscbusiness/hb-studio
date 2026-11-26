#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Sans-IO TLS client state machine. It never touches a socket: TlsStream (tls_stream.hpp) shuffles
// bytes between an hb::rtsp::ITcpStream and this engine. That split keeps the I/O loop (partial
// writes, WouldBlock, buffering) portable and unit-tested with a fake engine, while the only
// platform code is the engine itself (Schannel on Windows: tls_schannel.cpp).
//
// Contract: certificate verification and host-name matching against the `serverName` passed to
// begin() are ALWAYS on. There is deliberately no switch to turn them off.
namespace hb::streaming {

enum class TlsStatus {
    Ok,             // handshake: complete / decrypt: consumed one record (plaintext may be empty)
    NeedMoreInput,  // incomplete record: read more ciphertext and call again
    Closed,         // peer sent close_notify
    Error,          // fatal; see lastError()
};

class ITlsEngine {
public:
    virtual ~ITlsEngine() = default;

    // Starts a client session for `serverName` (SNI + certificate name check). False = cannot start.
    virtual bool begin(const std::string& serverName) = 0;

    // One handshake step. Consumes bytes from the FRONT of `in` (erasing what it used; leftover bytes
    // - e.g. the first application records - stay) and APPENDS any bytes that must be sent to `out`.
    // The first call is made with an empty `in` (produces the ClientHello). Returns Ok when the
    // handshake is complete (`out` may still hold the final flight to send), NeedMoreInput otherwise.
    virtual TlsStatus handshake(std::vector<std::uint8_t>& in, std::vector<std::uint8_t>& out) = 0;

    // Largest plaintext chunk encrypt() accepts in one call (TLS record payload limit).
    virtual std::size_t maxPlaintext() const = 0;
    // Encrypts n <= maxPlaintext() bytes into ONE record appended to `out`.
    virtual TlsStatus encrypt(const std::uint8_t* plain, std::size_t n, std::vector<std::uint8_t>& out) = 0;
    // Decrypts at most one record from the front of `in` (erasing it), appending plaintext to `plain`.
    // Post-handshake protocol messages that need a reply (e.g. TLS 1.3 key update) append to `out`.
    virtual TlsStatus decrypt(std::vector<std::uint8_t>& in, std::vector<std::uint8_t>& plain,
                              std::vector<std::uint8_t>& out) = 0;
    // Appends a close_notify alert to `out` (best effort).
    virtual void shutdown(std::vector<std::uint8_t>& out) = 0;

    virtual std::string lastError() const = 0;          // pt-BR
    // True when the failure is a verification failure (bad/expired/mismatched certificate, untrusted
    // root): retrying will not help and must not be hidden from the operator.
    virtual bool certificateError() const = 0;
};

// The platform engine: Schannel on Windows. nullptr where no TLS backend exists (Linux build).
std::unique_ptr<ITlsEngine> makeSystemTlsEngine();

}  // namespace hb::streaming
