#include "rtsp/rtp.hpp"

namespace hb::rtsp {

namespace {
void put16(Bytes& b, std::uint32_t v) {
    b.push_back(static_cast<std::uint8_t>(v >> 8));
    b.push_back(static_cast<std::uint8_t>(v));
}
void put32(Bytes& b, std::uint32_t v) {
    put16(b, v >> 16);
    put16(b, v & 0xffff);
}
std::uint32_t get32(const std::uint8_t* p) {
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | p[3];
}
}  // namespace

std::optional<RtpPacketView> parseRtp(ByteView d) {
    if (d.size() < 12) return std::nullopt;
    if ((d[0] >> 6) != 2) return std::nullopt;
    bool padding = d[0] & 0x20;
    bool ext = d[0] & 0x10;
    std::size_t cc = d[0] & 0x0f;
    std::size_t off = 12 + cc * 4;
    if (d.size() < off) return std::nullopt;
    if (ext) {
        if (d.size() < off + 4) return std::nullopt;
        std::size_t words = (std::size_t(d[off + 2]) << 8) | d[off + 3];
        off += 4 + words * 4;
        if (d.size() < off) return std::nullopt;
    }
    std::size_t end = d.size();
    if (padding) {
        std::size_t pad = d[d.size() - 1];
        if (pad == 0 || off + pad > d.size()) return std::nullopt;
        end -= pad;
    }
    RtpPacketView v;
    v.marker = d[1] & 0x80;
    v.payloadType = d[1] & 0x7f;
    v.sequence = static_cast<std::uint16_t>((d[2] << 8) | d[3]);
    v.timestamp = get32(d.data() + 4);
    v.ssrc = get32(d.data() + 8);
    v.payload = d.subspan(off, end - off);
    return v;
}

Bytes buildRtp(std::uint8_t pt, bool marker, std::uint16_t seq, std::uint32_t ts, std::uint32_t ssrc,
               ByteView payload) {
    Bytes b;
    b.reserve(12 + payload.size());
    b.push_back(0x80);
    b.push_back(static_cast<std::uint8_t>((marker ? 0x80 : 0) | (pt & 0x7f)));
    put16(b, seq);
    put32(b, ts);
    put32(b, ssrc);
    b.insert(b.end(), payload.begin(), payload.end());
    return b;
}

RtcpInfo parseRtcp(ByteView d) {
    RtcpInfo info;
    std::size_t p = 0;
    while (p + 4 <= d.size()) {
        if ((d[p] >> 6) != 2) { info.malformed = true; break; }
        std::uint8_t pt = d[p + 1];
        std::size_t len = ((std::size_t(d[p + 2]) << 8) | d[p + 3]) * 4 + 4;
        if (p + len > d.size()) { info.malformed = true; break; }
        if (pt == 200 && len >= 28) {
            SenderReportInfo s;
            s.ssrc = get32(&d[p + 4]);
            s.ntpMsw = get32(&d[p + 8]);
            s.ntpLsw = get32(&d[p + 12]);
            s.rtpTimestamp = get32(&d[p + 16]);
            s.packetCount = get32(&d[p + 20]);
            s.octetCount = get32(&d[p + 24]);
            info.senderReport = s;
        } else if (pt == 203) {
            info.bye = true;
        }
        p += len;
    }
    return info;
}

Bytes buildReceiverReport(std::uint32_t localSsrc, const ReportBlock& b, const std::string& cname) {
    Bytes o;
    o.push_back(0x81);  // V=2 P=0 RC=1
    o.push_back(201);
    put16(o, 7);  // 8 words total -> length 7
    put32(o, localSsrc);
    put32(o, b.sourceSsrc);
    o.push_back(b.fractionLost);
    std::uint32_t lost = b.cumulativeLost > 0xffffff ? 0xffffff : b.cumulativeLost;
    o.push_back(static_cast<std::uint8_t>(lost >> 16));
    o.push_back(static_cast<std::uint8_t>(lost >> 8));
    o.push_back(static_cast<std::uint8_t>(lost));
    put32(o, b.extHighestSeq);
    put32(o, b.jitter);
    put32(o, b.lastSr);
    put32(o, b.delaySinceLastSr);
    // SDES: header(4) + ssrc(4) + [type(1) len(1) text] + end(1) padded to 32 bits
    std::string name = cname.substr(0, 255);
    std::size_t body = 4 + 2 + name.size() + 1;
    std::size_t padded = (body + 3) / 4 * 4;
    o.push_back(0x81);
    o.push_back(202);
    put16(o, static_cast<std::uint32_t>((4 + padded) / 4 - 1));
    put32(o, localSsrc);
    o.push_back(1);
    o.push_back(static_cast<std::uint8_t>(name.size()));
    o.insert(o.end(), name.begin(), name.end());
    for (std::size_t i = 4 + 2 + name.size(); i < padded; ++i) o.push_back(0);
    return o;
}

Bytes buildSenderReport(const SenderReportInfo& s) {
    Bytes o;
    o.push_back(0x80);
    o.push_back(200);
    put16(o, 6);
    put32(o, s.ssrc);
    put32(o, s.ntpMsw);
    put32(o, s.ntpLsw);
    put32(o, s.rtpTimestamp);
    put32(o, s.packetCount);
    put32(o, s.octetCount);
    return o;
}

}  // namespace hb::rtsp
