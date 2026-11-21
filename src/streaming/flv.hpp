#pragma once
#include <cstdint>
#include <vector>

// Builds FLV-style audio/video tag BODIES (the payload RTMP's video (type 9) and audio (type 8)
// messages carry - RTMP reuses the exact FLV tag body layout, it just skips FLV's own file framing
// such as the previous-tag-size field). Pure byte-layout code: no I/O, no codec work - the caller
// already has encoded H.264 NAL units / raw AAC frames from a real encoder.
namespace hb::streaming::flv {

using Bytes = std::vector<std::uint8_t>;
using NalUnit = Bytes;   // one NAL unit, WITHOUT a 00 00 00 01 / 00 00 01 start code

// AVCDecoderConfigurationRecord wrapped in an FLV AVC sequence-header video tag body (AVCPacketType
// 0). Must be sent once, before the first frame, and again whenever SPS/PPS change (rare: a
// resolution/profile change mid-stream). `sps`/`pps` are raw, without start codes.
Bytes buildAvcSequenceHeader(const NalUnit& sps, const NalUnit& pps);

// One AVC NALU video tag body (AVCPacketType 1): `nalus` are the NAL units of a single frame
// (typically one, or SPS+PPS+IDR together for a keyframe some encoders emit per-frame - callers
// may also just send the slice NALU and rely on the sequence header above for SPS/PPS).
// `compositionTimeMs` is PTS-DTS, 0 unless the encoder reorders frames (B-frames) - this streamer's
// encoders are not expected to use B-frames, so 0 is the expected value in practice.
Bytes buildAvcVideoTag(const std::vector<NalUnit>& nalus, bool keyframe, std::int32_t compositionTimeMs = 0);

// AudioSpecificConfig wrapped in an FLV AAC sequence-header audio tag body (AACPacketType 0). Must
// be sent once, before the first audio tag. `sampleRate`/`channels` select the FLV "sound format"
// byte's rate/size/type bits (AAC itself always reports 44 kHz/16-bit/stereo in that byte per the
// FLV spec regardless of the real rate, which is what this always emits - the real rate lives in
// the AudioSpecificConfig bytes themselves, which is what players actually honour for AAC).
Bytes buildAacSequenceHeader(const Bytes& audioSpecificConfig);

// One raw AAC frame (ADTS-free, i.e. just the raw_data_block) audio tag body (AACPacketType 1).
Bytes buildAacAudioTag(const Bytes& aacRawFrame);

}  // namespace hb::streaming::flv
