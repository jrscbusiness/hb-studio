#include "ptz/visca.hpp"

#include <algorithm>
#include <cmath>

namespace hb::ptz::visca {

namespace {
double mag(double v) {
    v = std::fabs(v);
    if (!(v == v)) return 0;  // NaN
    return v > 1 ? 1 : v;
}
std::uint8_t scale(double m01, int maxV) {
    return static_cast<std::uint8_t>(1 + static_cast<int>(std::lround(mag(m01) * (maxV - 1))));
}
void nibbles(Bytes& b, int value) {
    const auto u = static_cast<std::uint16_t>(value);
    b.push_back((u >> 12) & 0xF);
    b.push_back((u >> 8) & 0xF);
    b.push_back((u >> 4) & 0xF);
    b.push_back(u & 0xF);
}
Bytes cmd(int address, std::initializer_list<int> body) {
    Bytes b{addrByte(address)};
    for (int x : body) b.push_back(static_cast<std::uint8_t>(x));
    b.push_back(0xFF);
    return b;
}
int clampPreset(int n) { return std::clamp(n, 0, 255); }
}  // namespace

std::uint8_t panSpeedByte(double m) { return scale(m, kMaxPanSpeed); }
std::uint8_t tiltSpeedByte(double m) { return scale(m, kMaxTiltSpeed); }
std::uint8_t zoomSpeedNibble(double m) {
    return static_cast<std::uint8_t>(std::lround(mag(m) * kMaxZoomSpeed));
}
std::uint8_t addrByte(int address) {
    return static_cast<std::uint8_t>(0x80 | (std::clamp(address, 0, 7) & 0x0F));
}

Bytes panTiltDrive(int address, double pan, double tilt) {
    const int h = pan < 0 ? 0x01 : (pan > 0 ? 0x02 : 0x03);
    const int v = tilt > 0 ? 0x01 : (tilt < 0 ? 0x02 : 0x03);
    const int vv = pan == 0 ? 1 : panSpeedByte(pan);
    const int ww = tilt == 0 ? 1 : tiltSpeedByte(tilt);
    return cmd(address, {0x01, 0x06, 0x01, vv, ww, h, v});
}
Bytes panTiltStop(int address) { return panTiltDrive(address, 0, 0); }

Bytes zoomDrive(int address, double zoom) {
    if (zoom == 0) return cmd(address, {0x01, 0x04, 0x07, 0x00});
    const int base = zoom > 0 ? 0x20 : 0x30;
    return cmd(address, {0x01, 0x04, 0x07, base | zoomSpeedNibble(zoom)});
}

Bytes presetSet(int a, int n) { return cmd(a, {0x01, 0x04, 0x3F, 0x01, clampPreset(n)}); }
Bytes presetRecall(int a, int n) { return cmd(a, {0x01, 0x04, 0x3F, 0x02, clampPreset(n)}); }
Bytes presetReset(int a, int n) { return cmd(a, {0x01, 0x04, 0x3F, 0x00, clampPreset(n)}); }

Bytes focusAuto(int a) { return cmd(a, {0x01, 0x04, 0x38, 0x02}); }
Bytes focusManual(int a) { return cmd(a, {0x01, 0x04, 0x38, 0x03}); }
Bytes focusStop(int a) { return cmd(a, {0x01, 0x04, 0x08, 0x00}); }
Bytes focusFar(int a, double s) { return cmd(a, {0x01, 0x04, 0x08, 0x20 | zoomSpeedNibble(s)}); }
Bytes focusNear(int a, double s) { return cmd(a, {0x01, 0x04, 0x08, 0x30 | zoomSpeedNibble(s)}); }
Bytes irisUp(int a) { return cmd(a, {0x01, 0x04, 0x0B, 0x02}); }
Bytes irisDown(int a) { return cmd(a, {0x01, 0x04, 0x0B, 0x03}); }

Bytes panTiltAbsolute(int a, double ps, double ts, int panRaw, int tiltRaw) {
    Bytes b{addrByte(a), 0x01, 0x06, 0x02, panSpeedByte(ps), tiltSpeedByte(ts)};
    nibbles(b, panRaw);
    nibbles(b, tiltRaw);
    b.push_back(0xFF);
    return b;
}
Bytes zoomDirect(int a, int zoomRaw) {
    Bytes b{addrByte(a), 0x01, 0x04, 0x47};
    nibbles(b, zoomRaw);
    b.push_back(0xFF);
    return b;
}
Bytes inquiryPanTiltPosition(int a) { return cmd(a, {0x09, 0x06, 0x12}); }
Bytes inquiryZoomPosition(int a) { return cmd(a, {0x09, 0x04, 0x47}); }
Bytes addressSetBroadcast() { return Bytes{0x88, 0x30, 0x01, 0xFF}; }

std::optional<Message> parseMessage(const Bytes& f) {
    if (f.size() < 3 || f.back() != 0xFF || !(f[0] & 0x80)) return std::nullopt;
    Message m;
    m.payload = f;
    const int hi = f[1] >> 4, lo = f[1] & 0x0F;
    if (hi == 0x4 && f.size() == 3) {
        m.kind = Message::Kind::Ack;
        m.socket = lo;
    } else if (hi == 0x5 && f.size() == 3) {
        m.kind = Message::Kind::Completion;
        m.socket = lo;
    } else if (hi == 0x5) {
        m.kind = Message::Kind::InquiryReply;  // y0 50 <data> FF
        m.socket = lo;
    } else if (hi == 0x6 && f.size() == 4) {
        m.kind = Message::Kind::Error;
        m.socket = lo;
        m.errorCode = f[2];
    }
    return m;
}

std::vector<Bytes> extractFrames(Bytes& buf) {
    std::vector<Bytes> out;
    std::size_t start = 0;
    for (std::size_t i = 0; i < buf.size(); ++i) {
        if (buf[i] == 0xFF) {
            out.emplace_back(buf.begin() + static_cast<std::ptrdiff_t>(start),
                             buf.begin() + static_cast<std::ptrdiff_t>(i) + 1);
            start = i + 1;
        }
    }
    buf.erase(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(start));
    if (buf.size() > 64) buf.clear();  // garbage without terminator: drop
    return out;
}

static int decode16(const Bytes& p, std::size_t at) {
    const unsigned v = ((p[at] & 0xFu) << 12) | ((p[at + 1] & 0xFu) << 8) |
                       ((p[at + 2] & 0xFu) << 4) | (p[at + 3] & 0xFu);
    return static_cast<std::int16_t>(static_cast<std::uint16_t>(v));
}

bool parsePanTiltPosition(const Message& m, int& pan, int& tilt) {
    if (m.kind != Message::Kind::InquiryReply || m.payload.size() != 11) return false;
    pan = decode16(m.payload, 2);
    tilt = decode16(m.payload, 6);
    return true;
}
bool parseZoomPosition(const Message& m, int& zoom) {
    if (m.kind != Message::Kind::InquiryReply || m.payload.size() != 7) return false;
    zoom = static_cast<std::uint16_t>(decode16(m.payload, 2));
    return true;
}

const char* errorName(int c) {
    switch (c) {
        case 0x01: return "message length error";
        case 0x02: return "syntax error";
        case 0x03: return "command buffer full";
        case 0x04: return "command cancelled";
        case 0x05: return "no socket";
        case 0x41: return "command not executable";
    }
    return "unknown VISCA error";
}

Bytes wrapIp(IpType type, std::uint32_t seq, const Bytes& payload) {
    const auto t = static_cast<std::uint16_t>(type);
    const auto len = static_cast<std::uint16_t>(payload.size());
    Bytes b{static_cast<std::uint8_t>(t >> 8), static_cast<std::uint8_t>(t),
            static_cast<std::uint8_t>(len >> 8), static_cast<std::uint8_t>(len),
            static_cast<std::uint8_t>(seq >> 24), static_cast<std::uint8_t>(seq >> 16),
            static_cast<std::uint8_t>(seq >> 8), static_cast<std::uint8_t>(seq)};
    b.insert(b.end(), payload.begin(), payload.end());
    return b;
}

std::optional<IpFrame> unwrapIp(const Bytes& d) {
    if (d.size() < 8) return std::nullopt;
    const std::size_t len = (static_cast<std::size_t>(d[2]) << 8) | d[3];
    if (len > 16 || d.size() != 8 + len) return std::nullopt;  // VISCA payloads are <= 16 bytes
    IpFrame f;
    f.type = static_cast<std::uint16_t>((d[0] << 8) | d[1]);
    f.sequence = (static_cast<std::uint32_t>(d[4]) << 24) | (static_cast<std::uint32_t>(d[5]) << 16) |
                 (static_cast<std::uint32_t>(d[6]) << 8) | d[7];
    f.payload.assign(d.begin() + 8, d.end());
    return f;
}

Bytes ipSequenceReset(std::uint32_t seq) { return wrapIp(IpType::Control, seq, Bytes{0x01}); }

}  // namespace hb::ptz::visca
