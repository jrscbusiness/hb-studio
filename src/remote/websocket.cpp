#include "remote/websocket.hpp"

#include <algorithm>

#include "remote/sha1.hpp"
#include "rtsp/base64.hpp"

namespace hb::remote::ws {

std::string acceptKey(std::string_view clientKey) {
    static constexpr std::string_view kGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    std::string cat(clientKey);
    cat += kGuid;
    const auto d = sha1(cat);
    return hb::rtsp::base64Encode(hb::rtsp::ByteView(d.data(), d.size()));
}

bool validClientKey(std::string_view key) {
    if (key.size() != 24) return false;  // 16 bytes -> 24 base64 chars with "==" padding
    const auto dec = hb::rtsp::base64Decode(key);
    return dec && dec->size() == 16 && key.substr(22) == "==";
}

bool validUtf8(std::string_view s) {
    std::size_t i = 0;
    const std::size_t n = s.size();
    while (i < n) {
        const auto c = static_cast<std::uint8_t>(s[i]);
        if (c < 0x80) { ++i; continue; }
        std::size_t need;
        std::uint32_t cp;
        if (c >= 0xC2 && c <= 0xDF) { need = 1; cp = c & 0x1Fu; }
        else if (c >= 0xE0 && c <= 0xEF) { need = 2; cp = c & 0x0Fu; }
        else if (c >= 0xF0 && c <= 0xF4) { need = 3; cp = c & 0x07u; }
        else return false;
        if (i + need >= n) return false;  // truncated sequence
        for (std::size_t k = 1; k <= need; ++k) {
            const auto cc = static_cast<std::uint8_t>(s[i + k]);
            if ((cc & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (cc & 0x3Fu);
        }
        if (need == 2 && (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF))) return false;
        if (need == 3 && (cp < 0x10000 || cp > 0x10FFFF)) return false;
        i += need + 1;
    }
    return true;
}

Parser::Result Parser::fail(std::uint16_t code, const char* why) {
    failed_ = true;
    failure_.ok = false;
    failure_.closeCode = code;
    failure_.reason = why;
    buf_.clear();
    pos_ = 0;
    return failure_;
}

Parser::Result Parser::feed(const std::uint8_t* data, std::size_t len, std::vector<Message>& out) {
    if (failed_) return failure_;
    // Bound the raw buffer: header (<= 14 bytes) + one frame payload (<= limit) + slack for the next
    // frame's bytes that arrived in the same read. A peer cannot make us hold more than that.
    if (buf_.size() - pos_ + len > limits_.maxMessageBytes * 2 + 64 * 1024) return fail(1009, "input too large");
    buf_.insert(buf_.end(), data, data + len);
    for (;;) {
        const std::size_t avail = buf_.size() - pos_;
        if (avail < 2) break;
        const std::uint8_t* p = buf_.data() + pos_;
        const bool fin = (p[0] & 0x80) != 0;
        if ((p[0] & 0x70) != 0) return fail(1002, "reserved bits set");
        const auto op = static_cast<std::uint8_t>(p[0] & 0x0F);
        const bool masked = (p[1] & 0x80) != 0;
        std::uint64_t plen = p[1] & 0x7Fu;
        std::size_t hdr = 2;
        if (plen == 126) {
            if (avail < 4) break;
            plen = (static_cast<std::uint64_t>(p[2]) << 8) | p[3];
            hdr = 4;
            if (plen < 126) return fail(1002, "non-minimal length");
        } else if (plen == 127) {
            if (avail < 10) break;
            plen = 0;
            for (int i = 0; i < 8; ++i) plen = (plen << 8) | p[2 + i];
            hdr = 10;
            if ((plen >> 63) != 0 || plen < 65536) return fail(1002, "bad 64-bit length");
        }
        const bool control = (op & 0x08) != 0;
        if (op != 0 && op != 1 && op != 2 && op != 8 && op != 9 && op != 10) return fail(1002, "unknown opcode");
        if (control && (!fin || plen > 125)) return fail(1002, "bad control frame");
        if (plen > limits_.maxMessageBytes) return fail(1009, "frame too large");
        if (requireMasked_ && !masked) return fail(1002, "client frame not masked");
        const std::size_t maskLen = masked ? 4 : 0;
        if (avail < hdr + maskLen + plen) break;  // wait for the rest of the frame
        std::string payload(static_cast<std::size_t>(plen), '\0');
        const std::uint8_t* src = p + hdr + maskLen;
        if (masked) {
            const std::uint8_t* mk = p + hdr;
            for (std::size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<char>(src[i] ^ mk[i & 3]);
        } else {
            std::copy(src, src + payload.size(), payload.begin());
        }
        pos_ += hdr + maskLen + static_cast<std::size_t>(plen);

        if (control) {
            Message m;
            if (op == 8) {
                m.type = Opcode::Close;
                if (payload.size() == 1) return fail(1002, "bad close payload");
                if (payload.size() >= 2) {
                    m.closeCode = static_cast<std::uint16_t>((static_cast<std::uint8_t>(payload[0]) << 8) | static_cast<std::uint8_t>(payload[1]));
                    const bool okCode = (m.closeCode >= 1000 && m.closeCode <= 1003) || (m.closeCode >= 1007 && m.closeCode <= 1011) ||
                                        (m.closeCode >= 3000 && m.closeCode <= 4999);
                    if (!okCode) return fail(1002, "bad close code");
                    m.data = payload.substr(2);
                    if (!validUtf8(m.data)) return fail(1007, "bad utf-8 in close");
                }
            } else {
                m.type = op == 9 ? Opcode::Ping : Opcode::Pong;
                m.data = std::move(payload);
            }
            out.push_back(std::move(m));
            continue;
        }
        // Data frame (possibly a fragment).
        if (op == 0) {
            if (!inFragment_) return fail(1002, "unexpected continuation");
            if (fragData_.size() + payload.size() > limits_.maxMessageBytes) return fail(1009, "message too large");
            fragData_ += payload;
        } else {
            if (inFragment_) return fail(1002, "new data frame inside fragmented message");
            fragType_ = op == 1 ? Opcode::Text : Opcode::Binary;
            fragData_ = std::move(payload);
            inFragment_ = true;
        }
        if (fin) {
            inFragment_ = false;
            Message m;
            m.type = fragType_;
            m.data = std::move(fragData_);
            fragData_.clear();
            if (m.type == Opcode::Text && !validUtf8(m.data)) return fail(1007, "invalid utf-8");
            out.push_back(std::move(m));
        }
    }
    // Compact consumed bytes.
    if (pos_ > 0 && (pos_ == buf_.size() || pos_ > 4096)) {
        buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(pos_));
        pos_ = 0;
    }
    return {};
}

static void putHeader(std::vector<std::uint8_t>& f, std::uint8_t b0, std::size_t len, bool masked) {
    f.push_back(b0);
    const std::uint8_t m = masked ? 0x80 : 0;
    if (len < 126) {
        f.push_back(static_cast<std::uint8_t>(m | len));
    } else if (len <= 0xFFFF) {
        f.push_back(static_cast<std::uint8_t>(m | 126));
        f.push_back(static_cast<std::uint8_t>(len >> 8));
        f.push_back(static_cast<std::uint8_t>(len));
    } else {
        f.push_back(static_cast<std::uint8_t>(m | 127));
        for (int i = 7; i >= 0; --i) f.push_back(static_cast<std::uint8_t>(static_cast<std::uint64_t>(len) >> (8 * i)));
    }
}

std::vector<std::uint8_t> encodeFrame(Opcode op, const std::uint8_t* payload, std::size_t len) {
    std::vector<std::uint8_t> f;
    f.reserve(len + 10);
    putHeader(f, static_cast<std::uint8_t>(0x80 | static_cast<std::uint8_t>(op)), len, false);
    if (len) f.insert(f.end(), payload, payload + len);
    return f;
}

std::vector<std::uint8_t> encodeClose(std::uint16_t code, std::string_view reason) {
    std::vector<std::uint8_t> p;
    p.push_back(static_cast<std::uint8_t>(code >> 8));
    p.push_back(static_cast<std::uint8_t>(code));
    const std::size_t n = std::min<std::size_t>(reason.size(), 100);  // control payload <= 125
    p.insert(p.end(), reason.begin(), reason.begin() + static_cast<std::ptrdiff_t>(n));
    return encodeFrame(Opcode::Close, p.data(), p.size());
}

std::vector<std::uint8_t> encodeMaskedFrame(Opcode op, const std::uint8_t* payload, std::size_t len, const std::uint8_t maskKey[4],
                                            bool fin) {
    std::vector<std::uint8_t> f;
    f.reserve(len + 14);
    putHeader(f, static_cast<std::uint8_t>((fin ? 0x80 : 0) | static_cast<std::uint8_t>(op)), len, true);
    f.insert(f.end(), maskKey, maskKey + 4);
    for (std::size_t i = 0; i < len; ++i) f.push_back(static_cast<std::uint8_t>(payload[i] ^ maskKey[i & 3]));
    return f;
}

}  // namespace hb::remote::ws
