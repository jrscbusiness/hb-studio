#include "rtsp/base64.hpp"

namespace hb::rtsp {

std::string base64Encode(ByteView d) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((d.size() + 2) / 3 * 4);
    for (std::size_t i = 0; i < d.size(); i += 3) {
        std::uint32_t v = std::uint32_t(d[i]) << 16;
        if (i + 1 < d.size()) v |= std::uint32_t(d[i + 1]) << 8;
        if (i + 2 < d.size()) v |= d[i + 2];
        out += t[(v >> 18) & 63];
        out += t[(v >> 12) & 63];
        out += i + 1 < d.size() ? t[(v >> 6) & 63] : '=';
        out += i + 2 < d.size() ? t[v & 63] : '=';
    }
    return out;
}

std::optional<Bytes> base64Decode(std::string_view text) {
    Bytes out;
    std::uint32_t acc = 0;
    int bits = 0;
    for (char c : text) {
        int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+' || c == '-') v = 62;
        else if (c == '/' || c == '_') v = 63;
        else if (c == '=') break;
        else if (c == ' ' || c == '\r' || c == '\n' || c == '\t') continue;
        else return std::nullopt;
        acc = ((acc << 6) | std::uint32_t(v)) & 0xffffff;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<std::uint8_t>((acc >> bits) & 0xff));
        }
    }
    return out;
}

}  // namespace hb::rtsp
