#pragma once
#include <optional>

#include "rtsp/types.hpp"

namespace hb::rtsp {

// Zero-copy view into the buffer passed to parseRtp (valid only while that buffer lives).
struct RtpPacketView {
    std::uint8_t payloadType = 0;
    bool marker = false;
    std::uint16_t sequence = 0;
    std::uint32_t timestamp = 0;
    std::uint32_t ssrc = 0;
    ByteView payload;
};

// Strict validation (version 2, header/CSRC/extension/padding must fit). nullopt on any
// malformed or truncated packet; never reads out of bounds.
std::optional<RtpPacketView> parseRtp(ByteView data);

// Builder used by tests and the fake server.
Bytes buildRtp(std::uint8_t pt, bool marker, std::uint16_t seq, std::uint32_t ts, std::uint32_t ssrc,
               ByteView payload);

// ---- RTCP (RFC 3550 section 6) ----
struct SenderReportInfo {
    std::uint32_t ssrc = 0;
    std::uint32_t ntpMsw = 0, ntpLsw = 0;
    std::uint32_t rtpTimestamp = 0;
    std::uint32_t packetCount = 0, octetCount = 0;
};
struct RtcpInfo {
    std::optional<SenderReportInfo> senderReport;  // last SR in the compound packet
    bool bye = false;
    bool malformed = false;
};
RtcpInfo parseRtcp(ByteView data);

struct ReportBlock {
    std::uint32_t sourceSsrc = 0;
    std::uint8_t fractionLost = 0;       // 8-bit fixed point
    std::uint32_t cumulativeLost = 0;    // clamped to 24 bits
    std::uint32_t extHighestSeq = 0;
    std::uint32_t jitter = 0;
    std::uint32_t lastSr = 0;            // middle 32 bits of the SR NTP timestamp
    std::uint32_t delaySinceLastSr = 0;  // 1/65536 s
};
// Compound packet: RR with one block + SDES(CNAME), as RFC 3550 requires.
Bytes buildReceiverReport(std::uint32_t localSsrc, const ReportBlock& block, const std::string& cname);
Bytes buildSenderReport(const SenderReportInfo& sr);  // for tests / fake server

}  // namespace hb::rtsp
