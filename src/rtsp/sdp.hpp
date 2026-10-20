#pragma once
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "rtsp/types.hpp"

namespace hb::rtsp {

struct RtpMap {
    std::string encoding;  // "H264", "H265", "MPEG4-GENERIC"...
    int clockRate = 0;
    int channels = 0;
};

struct MediaDescription {
    std::string type;   // "video", "audio", "application"
    int port = 0;
    std::string proto;  // "RTP/AVP"
    std::vector<int> payloadTypes;
    std::string control;  // a=control (empty if absent)
    std::map<int, RtpMap> rtpmap;
    std::map<int, std::string> fmtp;
    double framerate = 0;  // a=framerate / a=x-framerate, 0 = unknown
    int width = 0, height = 0;  // a=x-dimensions / a=framesize / a=cliprect, 0 = unknown
};

struct SessionDescription {
    std::string sessionName;
    std::string control;  // session-level a=control
    std::vector<MediaDescription> media;
};

// Lenient parser (accepts LF-only line endings, ignores unknown lines). Fails (nullopt) only when
// the text is not SDP at all (no v= line) or exceeds sane limits.
std::optional<SessionDescription> parseSdp(std::string_view text);

// Decoded out-of-band parameter sets from a=fmtp, in the order they should be fed to a decoder.
struct VideoFmtp {
    std::vector<Bytes> paramSets;  // H.264: SPS,PPS...   H.265: VPS,SPS,PPS
    int packetizationMode = 0;     // H.264
    std::string profileLevelId;    // H.264
    int spropMaxDonDiff = 0;       // H.265 (non-zero needs DON handling, unsupported)
};
VideoFmtp parseH264Fmtp(std::string_view fmtp);
VideoFmtp parseH265Fmtp(std::string_view fmtp);

// First media of type video whose encoding is H264 or H265 (case-insensitive).
struct VideoSelection {
    const MediaDescription* media = nullptr;
    int payloadType = -1;
    Codec codec = Codec::H264;
    int clockRate = 90000;
    VideoFmtp fmtp;
};
std::optional<VideoSelection> selectVideo(const SessionDescription& sdp);

}  // namespace hb::rtsp
