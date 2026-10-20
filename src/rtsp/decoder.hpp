#pragma once
#include <string>

#include "rtsp/types.hpp"

namespace hb::rtsp {

// Decoder boundary. DECLARED ONLY: no implementation exists yet. Decoding will be provided later
// by FFmpeg (libavcodec) and/or Media Foundation behind this interface; the RTSP module's output
// is the stream of AccessUnit (Annex B, SPS/PPS injected before keyframes).
class IVideoDecoder {
public:
    virtual ~IVideoDecoder() = default;
    virtual bool open(Codec codec) = 0;
    // Returns false if the AU could not be decoded; the caller may then request a new keyframe
    // by reconnecting. Decoded frames are delivered through an implementation specific sink.
    virtual bool decode(const AccessUnit& au) = 0;
    virtual void flush() = 0;
    virtual void close() = 0;
    virtual std::string lastError() const = 0;
};

}  // namespace hb::rtsp
