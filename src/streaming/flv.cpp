#include "streaming/flv.hpp"

namespace hb::streaming::flv {

namespace {
void put16be(Bytes& out, std::size_t v) {
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>(v & 0xFF));
}
void put24be(Bytes& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>(v & 0xFF));
}
void put32be(Bytes& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>(v & 0xFF));
}
}  // namespace

Bytes buildAvcSequenceHeader(const NalUnit& sps, const NalUnit& pps) {
    Bytes b;
    b.push_back(0x17);   // frameType=1 (key), codecId=7 (AVC)
    b.push_back(0x00);   // AVCPacketType = sequence header
    put24be(b, 0);       // composition time: always 0 for a sequence header
    // AVCDecoderConfigurationRecord (ISO/IEC 14496-15)
    b.push_back(0x01);                                                    // configurationVersion
    b.push_back(sps.size() > 1 ? sps[1] : 0x42);                          // AVCProfileIndication
    b.push_back(sps.size() > 2 ? sps[2] : 0x00);                          // profile_compatibility
    b.push_back(sps.size() > 3 ? sps[3] : 0x1E);                          // AVCLevelIndication
    b.push_back(0xFF);                                                    // 111111 + lengthSizeMinusOne=3 (4-byte NALU length)
    b.push_back(0xE1);                                                    // 111 + numOfSPS=1
    put16be(b, sps.size());
    b.insert(b.end(), sps.begin(), sps.end());
    b.push_back(0x01);                                                    // numOfPPS=1
    put16be(b, pps.size());
    b.insert(b.end(), pps.begin(), pps.end());
    return b;
}

Bytes buildAvcVideoTag(const std::vector<NalUnit>& nalus, bool keyframe, std::int32_t compositionTimeMs) {
    Bytes b;
    b.push_back(keyframe ? 0x17 : 0x27);   // frameType (1=key/2=inter) <<4 | codecId=7 (AVC)
    b.push_back(0x01);                     // AVCPacketType = NALU
    put24be(b, static_cast<std::uint32_t>(compositionTimeMs) & 0x00FFFFFF);
    for (const auto& nalu : nalus) {
        put32be(b, static_cast<std::uint32_t>(nalu.size()));   // AVCC: 4-byte length prefix, no start code
        b.insert(b.end(), nalu.begin(), nalu.end());
    }
    return b;
}

Bytes buildAacSequenceHeader(const Bytes& audioSpecificConfig) {
    Bytes b;
    b.push_back(0xAF);   // soundFormat=10 (AAC), rate=3 (placeholder), size=1 (16-bit), type=1 (stereo)
    b.push_back(0x00);   // AACPacketType = sequence header (AudioSpecificConfig)
    b.insert(b.end(), audioSpecificConfig.begin(), audioSpecificConfig.end());
    return b;
}

Bytes buildAacAudioTag(const Bytes& aacRawFrame) {
    Bytes b;
    b.push_back(0xAF);
    b.push_back(0x01);   // AACPacketType = raw AAC frame
    b.insert(b.end(), aacRawFrame.begin(), aacRawFrame.end());
    return b;
}

}  // namespace hb::streaming::flv
