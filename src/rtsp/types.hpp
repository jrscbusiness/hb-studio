#pragma once
#include <chrono>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace hb::rtsp {

using Bytes = std::vector<std::uint8_t>;
using ByteView = std::span<const std::uint8_t>;
using TimePoint = std::chrono::steady_clock::time_point;
using Millis = std::chrono::milliseconds;

enum class Codec { H264, H265 };
const char* codecName(Codec c);

struct Credentials {
    std::string username;
    std::string password;
    bool empty() const { return username.empty(); }
};

// One encoded video frame (Annex B, start codes 00 00 00 01) ready for a decoder.
struct AccessUnit {
    Codec codec = Codec::H264;
    Bytes data;
    std::int64_t ptsUs = 0;          // relative to the first RTP timestamp of the stream
    std::uint32_t rtpTimestamp = 0;
    bool keyframe = false;           // contains IDR (H.264) / IRAP (H.265)
    bool damaged = false;            // a packet of this AU was lost (only emitted if configured)
    TimePoint completedAt{};         // injected-clock time the last packet arrived
};

inline Bytes toBytes(const std::string& s) { return Bytes(s.begin(), s.end()); }
inline std::string toString(ByteView v) { return std::string(v.begin(), v.end()); }

}  // namespace hb::rtsp
