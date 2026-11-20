#include "recording/md5.hpp"

#include <cstring>

namespace hb::rec::detail {

namespace {
const std::uint32_t K[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
const int R[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9,  14, 20, 5, 9,
                   14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                   4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
inline std::uint32_t rol(std::uint32_t x, int c) { return (x << c) | (x >> (32 - c)); }
}  // namespace

void Md5::reset() {
    s_[0] = 0x67452301;
    s_[1] = 0xefcdab89;
    s_[2] = 0x98badcfe;
    s_[3] = 0x10325476;
    bufLen_ = 0;
    total_ = 0;
}

void Md5::block(const std::uint8_t* p) {
    std::uint32_t m[16];
    for (int i = 0; i < 16; ++i)
        m[i] = std::uint32_t(p[i * 4]) | (std::uint32_t(p[i * 4 + 1]) << 8) | (std::uint32_t(p[i * 4 + 2]) << 16) |
               (std::uint32_t(p[i * 4 + 3]) << 24);
    std::uint32_t a = s_[0], b = s_[1], c = s_[2], d = s_[3];
    for (int i = 0; i < 64; ++i) {
        std::uint32_t f;
        int g;
        if (i < 16) {
            f = (b & c) | (~b & d);
            g = i;
        } else if (i < 32) {
            f = (d & b) | (~d & c);
            g = (5 * i + 1) & 15;
        } else if (i < 48) {
            f = b ^ c ^ d;
            g = (3 * i + 5) & 15;
        } else {
            f = c ^ (b | ~d);
            g = (7 * i) & 15;
        }
        const std::uint32_t t = d;
        d = c;
        c = b;
        b = b + rol(a + f + K[i] + m[g], R[i]);
        a = t;
    }
    s_[0] += a;
    s_[1] += b;
    s_[2] += c;
    s_[3] += d;
}

void Md5::update(const void* data, std::size_t len) {
    const auto* p = static_cast<const std::uint8_t*>(data);
    total_ += len;
    while (len > 0) {
        if (bufLen_ == 0 && len >= 64) {
            block(p);
            p += 64;
            len -= 64;
            continue;
        }
        const std::size_t n = (64 - bufLen_ < len) ? 64 - bufLen_ : len;
        std::memcpy(buf_ + bufLen_, p, n);
        bufLen_ += n;
        p += n;
        len -= n;
        if (bufLen_ == 64) {
            block(buf_);
            bufLen_ = 0;
        }
    }
}

std::array<std::uint8_t, 16> Md5::finish() {
    const std::uint64_t bits = total_ * 8;
    const std::uint8_t pad = 0x80;
    update(&pad, 1);
    const std::uint8_t zero = 0;
    while (bufLen_ != 56) update(&zero, 1);
    std::uint8_t lenBytes[8];
    for (int i = 0; i < 8; ++i) lenBytes[i] = static_cast<std::uint8_t>(bits >> (8 * i));
    update(lenBytes, 8);
    std::array<std::uint8_t, 16> out{};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) out[i * 4 + j] = static_cast<std::uint8_t>(s_[i] >> (8 * j));
    return out;
}

}  // namespace hb::rec::detail
