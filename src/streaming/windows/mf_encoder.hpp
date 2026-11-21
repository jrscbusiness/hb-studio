#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// Raw H.264/AAC encoding for TRANSMISSÃO (RTMP streaming), using Windows Media Foundation's
// MFT_CATEGORY_VIDEO_ENCODER / MFT_CATEGORY_AUDIO_ENCODER transforms DIRECTLY (IMFTransform), NOT
// IMFSinkWriter. This is a deliberate, separate path from src/recording/windows/mf_writers.cpp:
// IMFSinkWriter only muxes straight into an MP4 file with no exposed point to pull out a raw
// encoded access unit + timestamp before muxing (confirmed by trying; see mf_writers.cpp:64-75's
// own comment on this), which RTMP needs (it has its own container framing, FLV-style, built by
// src/streaming/flv.hpp). Reuse what's safe to reuse - encoder/media-type discovery patterns - but
// this is a new component, not a refactor of MfWriter.
//
// *** THIS HEADER IS THE FROZEN CONTRACT for the Media Foundation implementation
// (mf_video_encoder.cpp / mf_audio_encoder.cpp). Implement against it without changing its shape;
// extend only by adding new methods/fields, never by changing existing signatures, so the portable
// orchestration code (src/app/windows/stream_sender.*, not written against MF specifics) keeps
// compiling. ***
namespace hb::win {

struct EncodedVideoUnit {
    std::vector<std::uint8_t> nalu;   // one NAL unit, WITHOUT a start code (00 00 00 01 / 00 00 01)
    bool keyframe = false;
    std::uint32_t timestampMs = 0;    // relative to the first frame fed to encode()
};

struct EncodedAudioUnit {
    std::vector<std::uint8_t> raw;    // one raw AAC frame (no ADTS header - FLV/RTMP doesn't want one)
    std::uint32_t timestampMs = 0;
};

// One H.264 encoder instance bound to a fixed size/rate/bitrate for its lifetime (TRANSMISSÃO
// restarts the encoder rather than reconfiguring it on the rare occasion settings change mid-stream
// - exactly like DShowSource's format is fixed at construction).
class IH264Encoder {
public:
    virtual ~IH264Encoder() = default;

    // `encoderPref`: "auto" | "x264" (software) | "nvenc" | "qsv" | "amf" - a hint, not a guarantee;
    // implementations should fall back to any available H.264 MFT (logging/reporting which one was
    // actually used via lastError()/activeEncoderName() if the preferred one isn't present) rather
    // than failing outright, exactly like DShowSource falls back to automatic format negotiation.
    virtual bool start(int width, int height, int fpsNum, int fpsDen, int bitrateKbps, const std::string& encoderPref) = 0;
    virtual void stop() = 0;
    virtual bool isRunning() const = 0;

    // Feeds one BGRA8 frame (top-down, `stride` bytes/row - exactly video::Frame's layout).
    // `onUnit` is called zero or more times synchronously before encode() returns (an encoder may
    // buffer internally and emit 0 units for an early call, then several once its pipeline fills).
    // False only on a hard encoder failure (see lastError()); the caller should stop() and may
    // create a fresh encoder to retry - never call encode() again after a false return.
    virtual bool encode(const std::uint8_t* bgra, int width, int height, int stride, std::uint32_t timestampMs,
                        const std::function<void(const EncodedVideoUnit&)>& onUnit) = 0;

    // SPS and PPS (each WITHOUT a start code), available once encode() has produced its first unit;
    // empty before that. Feeds streaming::flv::buildAvcSequenceHeader().
    virtual bool spsPps(std::vector<std::uint8_t>& sps, std::vector<std::uint8_t>& pps) const = 0;

    virtual std::string activeEncoderName() const = 0;   // "x264" | "nvenc" | "qsv" | "amf", once known
    virtual std::string lastError() const = 0;
};

// One AAC encoder instance; input is interleaved float32 stereo at a fixed sample rate (matching
// hb::audio_io::AudioEngine's ProgramConsumer tap - see audio_engine.hpp - kProgramSampleRate=48000
// in practice).
class IAacEncoder {
public:
    virtual ~IAacEncoder() = default;

    virtual bool start(int sampleRate, int channels, int bitrateKbps) = 0;
    virtual void stop() = 0;
    virtual bool isRunning() const = 0;

    // `frames` is the number of interleaved sample frames in `stereo` (i.e. `stereo` holds
    // `frames * channels` floats). Same buffering/callback contract as IH264Encoder::encode().
    virtual bool encode(const float* interleaved, std::size_t frames, std::uint32_t timestampMs,
                        const std::function<void(const EncodedAudioUnit&)>& onUnit) = 0;

    // The raw AudioSpecificConfig bytes (ISO 14496-3), available once encode() has produced its
    // first unit. Feeds streaming::flv::buildAacSequenceHeader().
    virtual bool audioSpecificConfig(std::vector<std::uint8_t>& out) const = 0;

    virtual std::string lastError() const = 0;
};

// Real Media Foundation implementations (mf_video_encoder.cpp / mf_audio_encoder.cpp). Each
// IH264Encoder/IAacEncoder is meant to be used by exactly one StreamSender; create a fresh one per
// destination/attempt, never shared.
std::unique_ptr<IH264Encoder> makeH264Encoder();
std::unique_ptr<IAacEncoder> makeAacEncoder();

}  // namespace hb::win
