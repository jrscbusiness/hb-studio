#pragma once
// SHA-1 (RFC 3174). Needed ONLY for the WebSocket handshake (RFC 6455 section 4.2.2 mandates it for
// Sec-WebSocket-Accept); never used for anything security relevant here (tokens use SHA-256).
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace hb::remote {

std::array<std::uint8_t, 20> sha1(const void* data, std::size_t len);
inline std::array<std::uint8_t, 20> sha1(std::string_view s) { return sha1(s.data(), s.size()); }

}  // namespace hb::remote
