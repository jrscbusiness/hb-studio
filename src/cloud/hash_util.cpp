#include "cloud/hash_util.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "core/sha256.hpp"

namespace hb::cloud {

std::string hexEncode(std::string_view bytes) {
    static const char* d = "0123456789abcdef";
    std::string o;
    o.reserve(bytes.size() * 2);
    for (unsigned char c : bytes) {
        o += d[c >> 4];
        o += d[c & 15];
    }
    return o;
}

std::string hexDecode(std::string_view hex) {
    if (hex.size() % 2) return {};
    auto v = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string o;
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        const int a = v(hex[i]), b = v(hex[i + 1]);
        if (a < 0 || b < 0) return {};
        o += static_cast<char>(a * 16 + b);
    }
    return o;
}

std::string sha256Hex(std::string_view data) { return hb::core::Sha256::hex(data); }
std::string sha256Raw(std::string_view data) { return hexDecode(sha256Hex(data)); }

std::string hmacSha256(std::string_view key, std::string_view data) {
    std::string k(key);
    if (k.size() > 64) k = sha256Raw(k);
    k.resize(64, '\0');
    std::string ipad(64, '\0'), opad(64, '\0');
    for (std::size_t i = 0; i < 64; ++i) {
        ipad[i] = static_cast<char>(k[i] ^ 0x36);
        opad[i] = static_cast<char>(k[i] ^ 0x5c);
    }
    hb::core::Sha256 in;
    in.update(ipad);
    in.update(data);
    const std::string innerHash = hexDecode(in.hexDigest());
    hb::core::Sha256 out;
    out.update(opad);
    out.update(innerHash);
    return hexDecode(out.hexDigest());
}

std::string base64Encode(std::string_view data) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    std::size_t i = 0;
    for (; i + 2 < data.size(); i += 3) {
        const unsigned v = (static_cast<unsigned>(static_cast<unsigned char>(data[i])) << 16) |
                           (static_cast<unsigned>(static_cast<unsigned char>(data[i + 1])) << 8) |
                           static_cast<unsigned>(static_cast<unsigned char>(data[i + 2]));
        o += t[v >> 18];
        o += t[(v >> 12) & 63];
        o += t[(v >> 6) & 63];
        o += t[v & 63];
    }
    if (i + 1 == data.size()) {
        const unsigned v = static_cast<unsigned>(static_cast<unsigned char>(data[i])) << 16;
        o += t[v >> 18];
        o += t[(v >> 12) & 63];
        o += "==";
    } else if (i + 2 == data.size()) {
        const unsigned v = (static_cast<unsigned>(static_cast<unsigned char>(data[i])) << 16) |
                           (static_cast<unsigned>(static_cast<unsigned char>(data[i + 1])) << 8);
        o += t[v >> 18];
        o += t[(v >> 12) & 63];
        o += t[(v >> 6) & 63];
        o += '=';
    }
    return o;
}

std::string base64UrlEncode(std::string_view data) {
    std::string o = base64Encode(data);
    for (auto& c : o) {
        if (c == '+') c = '-';
        else if (c == '/') c = '_';
    }
    while (!o.empty() && o.back() == '=') o.pop_back();
    return o;
}

std::string base64Decode(std::string_view text) {
    auto v = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::string o;
    unsigned acc = 0;
    int bits = 0;
    for (char c : text) {
        if (c == '=') break;
        const int x = v(c);
        if (x < 0) return {};
        acc = (acc << 6) | static_cast<unsigned>(x);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            o += static_cast<char>((acc >> bits) & 0xff);
        }
    }
    return o;
}

// ---- MD5 ----
namespace {
inline std::uint32_t rol(std::uint32_t x, int c) { return (x << c) | (x >> (32 - c)); }
const std::uint32_t* md5K() {
    static const std::uint32_t* k = [] {
        static std::uint32_t t[64];
        for (int i = 0; i < 64; ++i)
            t[i] = static_cast<std::uint32_t>(std::floor(std::fabs(std::sin(static_cast<double>(i + 1))) * 4294967296.0));
        return t;
    }();
    return k;
}
}  // namespace

void Md5::reset() {
    s_[0] = 0x67452301u;
    s_[1] = 0xefcdab89u;
    s_[2] = 0x98badcfeu;
    s_[3] = 0x10325476u;
    bufLen_ = 0;
    total_ = 0;
}

void Md5::block(const std::uint8_t* p) {
    static const int r[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9,  14, 20, 5, 9,
                              14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                              4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
    const std::uint32_t* K = md5K();
    std::uint32_t m[16];
    for (int i = 0; i < 16; ++i)
        m[i] = static_cast<std::uint32_t>(p[i * 4]) | (static_cast<std::uint32_t>(p[i * 4 + 1]) << 8) |
               (static_cast<std::uint32_t>(p[i * 4 + 2]) << 16) | (static_cast<std::uint32_t>(p[i * 4 + 3]) << 24);
    std::uint32_t a = s_[0], b = s_[1], c = s_[2], d = s_[3];
    for (int i = 0; i < 64; ++i) {
        std::uint32_t f;
        int g;
        if (i < 16) {
            f = (b & c) | (~b & d);
            g = i;
        } else if (i < 32) {
            f = (d & b) | (~d & c);
            g = (5 * i + 1) % 16;
        } else if (i < 48) {
            f = b ^ c ^ d;
            g = (3 * i + 5) % 16;
        } else {
            f = c ^ (b | ~d);
            g = (7 * i) % 16;
        }
        const std::uint32_t tmp = d;
        d = c;
        c = b;
        b = b + rol(a + f + K[i] + m[g], r[i]);
        a = tmp;
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
        if (bufLen_ == 0 && len >= 64) {  // fast path: whole blocks straight from the input
            block(p);
            p += 64;
            len -= 64;
            continue;
        }
        const std::size_t n = std::min<std::size_t>(len, 64 - bufLen_);
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

std::string Md5::finishRaw() {
    const std::uint64_t bits = total_ * 8;
    const std::uint8_t pad = 0x80;
    update(&pad, 1);
    const std::uint8_t zero = 0;
    while (bufLen_ != 56) update(&zero, 1);
    std::uint8_t lenBytes[8];
    for (int i = 0; i < 8; ++i) lenBytes[i] = static_cast<std::uint8_t>(bits >> (8 * i));
    update(lenBytes, 8);
    std::string o(16, '\0');
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) o[static_cast<std::size_t>(i * 4 + j)] = static_cast<char>((s_[i] >> (8 * j)) & 0xff);
    return o;
}

std::string Md5::finishHex() { return hexEncode(finishRaw()); }

std::string md5Hex(std::string_view data) {
    Md5 m;
    m.update(data.data(), data.size());
    return m.finishHex();
}

std::string uriEncode(std::string_view s, bool encodeSlash) {
    static const char* hex = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~' ||
            (c == '/' && !encodeSlash)) {
            o += static_cast<char>(c);
        } else {
            o += '%';
            o += hex[c >> 4];
            o += hex[c & 15];
        }
    }
    return o;
}

}  // namespace hb::cloud
