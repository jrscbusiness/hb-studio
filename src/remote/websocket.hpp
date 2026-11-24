#pragma once
// WebSocket (RFC 6455) framing: server-side incremental parser + encoders + handshake key.
// Pure logic, no sockets. The parser is hostile-input safe: bounded frame/message sizes (checked
// BEFORE buffering), strict masking/RSV/opcode/control-frame rules, UTF-8 validation of text.
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace hb::remote::ws {

enum class Opcode : std::uint8_t { Continuation = 0x0, Text = 0x1, Binary = 0x2, Close = 0x8, Ping = 0x9, Pong = 0xA };

// Sec-WebSocket-Accept for a client Sec-WebSocket-Key (base64(SHA1(key + GUID))).
std::string acceptKey(std::string_view clientKey);
// True if `key` is a valid Sec-WebSocket-Key (base64 of exactly 16 bytes).
bool validClientKey(std::string_view key);

// Strict UTF-8 validation (rejects overlongs, surrogates, > U+10FFFF).
bool validUtf8(std::string_view s);

struct Message {
    Opcode type = Opcode::Text;     // Text, Binary, Close, Ping or Pong (never Continuation)
    std::string data;               // payload (for Close: the reason text only)
    std::uint16_t closeCode = 1005; // Close only: 1005 = none given
};

struct ParserLimits {
    std::size_t maxMessageBytes = 16 * 1024;  // reassembled message AND single frame payload cap
};

class Parser {
public:
    explicit Parser(ParserLimits limits = {}, bool requireMasked = true) : limits_(limits), requireMasked_(requireMasked) {}

    struct Result {
        bool ok = true;
        std::uint16_t closeCode = 0;  // when !ok: the close code the endpoint should send (1002, 1007, 1009...)
        std::string reason;
    };

    // Appends bytes and extracts every complete message into `out`. After a failure the parser is
    // poisoned (every later feed() fails the same way); the caller closes the connection.
    Result feed(const std::uint8_t* data, std::size_t len, std::vector<Message>& out);

private:
    Result fail(std::uint16_t code, const char* why);
    ParserLimits limits_;
    bool requireMasked_;
    std::vector<std::uint8_t> buf_;
    std::size_t pos_ = 0;
    Opcode fragType_ = Opcode::Text;
    bool inFragment_ = false;
    std::string fragData_;
    bool failed_ = false;
    Result failure_;
};

// Server -> client frame (FIN set, unmasked).
std::vector<std::uint8_t> encodeFrame(Opcode op, const std::uint8_t* payload, std::size_t len);
inline std::vector<std::uint8_t> encodeText(std::string_view s) {
    return encodeFrame(Opcode::Text, reinterpret_cast<const std::uint8_t*>(s.data()), s.size());
}
std::vector<std::uint8_t> encodeClose(std::uint16_t code, std::string_view reason = {});
// Client -> server frame (masked with `maskKey`); used by tests and by client-side tools.
std::vector<std::uint8_t> encodeMaskedFrame(Opcode op, const std::uint8_t* payload, std::size_t len, const std::uint8_t maskKey[4],
                                            bool fin = true);

}  // namespace hb::remote::ws
