#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace hb::onvif {

// Own SHA-1 (RFC 3174) and Base64 (RFC 4648). SHA-1 is used ONLY because
// WS-Security UsernameToken PasswordDigest mandates it; not for any new security purpose.
std::array<std::uint8_t, 20> sha1(std::string_view data);
std::string toHex(const std::uint8_t* data, std::size_t size);

std::string base64Encode(std::string_view data);
// Tolerates embedded whitespace/newlines and missing padding. nullopt on invalid characters.
std::optional<std::string> base64Decode(std::string_view text);

// Base64(SHA1(rawNonce + created + password)) per WS-Security UsernameToken Profile 1.0.
std::string passwordDigest(std::string_view rawNonce, std::string_view created, std::string_view password);

}  // namespace hb::onvif
