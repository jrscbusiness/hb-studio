#include "remote/sha1.hpp"

#include <cstring>

namespace hb::remote {

namespace {
inline std::uint32_t rol(std::uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

void block(std::uint32_t h[5], const std::uint8_t* p) {
    std::uint32_t w[80];
    for (int i = 0; i < 16; ++i)
        w[i] = (static_cast<std::uint32_t>(p[i * 4]) << 24) | (static_cast<std::uint32_t>(p[i * 4 + 1]) << 16) |
               (static_cast<std::uint32_t>(p[i * 4 + 2]) << 8) | static_cast<std::uint32_t>(p[i * 4 + 3]);
    for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; ++i) {
        std::uint32_t f, k;
        if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999u; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1u; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
        else { f = b ^ c ^ d; k = 0xCA62C1D6u; }
        const std::uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol(b, 30); b = a; a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}
}  // namespace

std::array<std::uint8_t, 20> sha1(const void* data, std::size_t len) {
    std::uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    const auto* p = static_cast<const std::uint8_t*>(data);
    const std::size_t full = len / 64;
    for (std::size_t i = 0; i < full; ++i) block(h, p + i * 64);
    std::uint8_t tail[128] = {0};
    const std::size_t rem = len - full * 64;
    if (rem) std::memcpy(tail, p + full * 64, rem);
    tail[rem] = 0x80;
    const std::size_t tailLen = rem + 1 + 8 <= 64 ? 64 : 128;
    const std::uint64_t bits = static_cast<std::uint64_t>(len) * 8;
    for (std::size_t i = 0; i < 8; ++i) tail[tailLen - 1 - i] = static_cast<std::uint8_t>(bits >> (8 * i));
    block(h, tail);
    if (tailLen == 128) block(h, tail + 64);
    std::array<std::uint8_t, 20> out{};
    for (std::size_t i = 0; i < 5; ++i)
        for (std::size_t j = 0; j < 4; ++j) out[i * 4 + j] = static_cast<std::uint8_t>(h[i] >> (24 - 8 * j));
    return out;
}

}  // namespace hb::remote
