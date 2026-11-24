#pragma once
// Small crypto/encoding helpers for the cloud module (own implementations: no dependency).
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace hb::cloud {

std::string hexEncode(std::string_view bytes);   // lowercase
std::string hexDecode(std::string_view hex);     // raw bytes; "" if odd length / invalid
std::string sha256Raw(std::string_view data);    // 32 raw bytes
std::string sha256Hex(std::string_view data);
std::string hmacSha256(std::string_view key, std::string_view data);  // 32 raw bytes (RFC 2104)
std::string base64Encode(std::string_view data);
std::string base64UrlEncode(std::string_view data);  // RFC 7636: no padding, '-' '_'
std::string base64Decode(std::string_view text);     // "" on invalid input

// RFC 1321 MD5 (S3 ETags, Drive md5Checksum). Not used for any security purpose.
class Md5 {
public:
    Md5() { reset(); }
    void reset();
    void update(const void* data, std::size_t len);
    std::string finishRaw();  // 16 raw bytes; reset() before reuse
    std::string finishHex();

private:
    void block(const std::uint8_t* p);
    std::uint32_t s_[4];
    std::uint8_t buf_[64];
    std::size_t bufLen_ = 0;
    std::uint64_t total_ = 0;
};
std::string md5Hex(std::string_view data);

// RFC 3986 percent-encoding as AWS wants it: unreserved (A-Za-z0-9-_.~) kept, everything else %XX
// (uppercase hex). '/' is kept unless encodeSlash.
std::string uriEncode(std::string_view s, bool encodeSlash);

}  // namespace hb::cloud
