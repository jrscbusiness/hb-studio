#include "onvif/crypto.hpp"

#include <cstring>

namespace hb::onvif {
namespace {

inline std::uint32_t rotl(std::uint32_t v, int s) { return (v << s) | (v >> (32 - s)); }

void processBlock(std::uint32_t h[5], const std::uint8_t* p) {
    std::uint32_t w[80];
    for (int i = 0; i < 16; ++i) {
        w[i] = (std::uint32_t(p[i * 4]) << 24) | (std::uint32_t(p[i * 4 + 1]) << 16) |
               (std::uint32_t(p[i * 4 + 2]) << 8) | std::uint32_t(p[i * 4 + 3]);
    }
    for (int i = 16; i < 80; ++i) w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; ++i) {
        std::uint32_t f, k;
        if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
        else { f = b ^ c ^ d; k = 0xCA62C1D6; }
        const std::uint32_t t = rotl(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rotl(b, 30); b = a; a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}

const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

}  // namespace

std::array<std::uint8_t, 20> sha1(std::string_view data) {
    std::uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    const auto* p = reinterpret_cast<const std::uint8_t*>(data.data());
    const std::size_t n = data.size();
    std::size_t off = 0;
    while (n - off >= 64) {
        processBlock(h, p + off);
        off += 64;
    }
    std::uint8_t tail[128] = {0};
    const std::size_t rem = n - off;
    if (rem) std::memcpy(tail, p + off, rem);
    tail[rem] = 0x80;
    const std::size_t total = (rem + 1 + 8 <= 64) ? 64 : 128;
    const std::uint64_t bits = static_cast<std::uint64_t>(n) * 8;
    for (int i = 0; i < 8; ++i) tail[total - 1 - i] = static_cast<std::uint8_t>(bits >> (8 * i));
    processBlock(h, tail);
    if (total == 128) processBlock(h, tail + 64);
    std::array<std::uint8_t, 20> out{};
    for (int i = 0; i < 5; ++i) {
        out[i * 4] = static_cast<std::uint8_t>(h[i] >> 24);
        out[i * 4 + 1] = static_cast<std::uint8_t>(h[i] >> 16);
        out[i * 4 + 2] = static_cast<std::uint8_t>(h[i] >> 8);
        out[i * 4 + 3] = static_cast<std::uint8_t>(h[i]);
    }
    return out;
}

std::string toHex(const std::uint8_t* data, std::size_t size) {
    static const char* d = "0123456789abcdef";
    std::string s;
    s.reserve(size * 2);
    for (std::size_t i = 0; i < size; ++i) {
        s.push_back(d[data[i] >> 4]);
        s.push_back(d[data[i] & 15]);
    }
    return s;
}

std::string base64Encode(std::string_view data) {
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    std::size_t i = 0;
    while (i + 2 < data.size()) {
        const std::uint32_t v =
            (std::uint32_t(std::uint8_t(data[i])) << 16) | (std::uint32_t(std::uint8_t(data[i + 1])) << 8) | std::uint8_t(data[i + 2]);
        out.push_back(kB64[v >> 18]);
        out.push_back(kB64[(v >> 12) & 63]);
        out.push_back(kB64[(v >> 6) & 63]);
        out.push_back(kB64[v & 63]);
        i += 3;
    }
    if (data.size() - i == 1) {
        const std::uint32_t v = std::uint32_t(std::uint8_t(data[i])) << 16;
        out.push_back(kB64[v >> 18]);
        out.push_back(kB64[(v >> 12) & 63]);
        out += "==";
    } else if (data.size() - i == 2) {
        const std::uint32_t v = (std::uint32_t(std::uint8_t(data[i])) << 16) | (std::uint32_t(std::uint8_t(data[i + 1])) << 8);
        out.push_back(kB64[v >> 18]);
        out.push_back(kB64[(v >> 12) & 63]);
        out.push_back(kB64[(v >> 6) & 63]);
        out.push_back('=');
    }
    return out;
}

std::optional<std::string> base64Decode(std::string_view text) {
    std::string out;
    std::uint32_t acc = 0;
    int bits = 0;
    bool padding = false;
    for (char ch : text) {
        if (ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t') continue;
        if (ch == '=') { padding = true; continue; }
        if (padding) return std::nullopt;  // data after padding
        int v;
        if (ch >= 'A' && ch <= 'Z') v = ch - 'A';
        else if (ch >= 'a' && ch <= 'z') v = ch - 'a' + 26;
        else if (ch >= '0' && ch <= '9') v = ch - '0' + 52;
        else if (ch == '+') v = 62;
        else if (ch == '/') v = 63;
        else return std::nullopt;
        acc = (acc << 6) | static_cast<std::uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((acc >> bits) & 0xFF));
        }
    }
    return out;
}

std::string passwordDigest(std::string_view rawNonce, std::string_view created, std::string_view password) {
    std::string buf;
    buf.reserve(rawNonce.size() + created.size() + password.size());
    buf.append(rawNonce).append(created).append(password);
    const auto d = sha1(buf);
    return base64Encode(std::string_view(reinterpret_cast<const char*>(d.data()), d.size()));
}

}  // namespace hb::onvif
