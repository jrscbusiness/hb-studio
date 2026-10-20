#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "rtsp/types.hpp"

namespace hb::rtsp {

struct RtspUrl {
    std::string host;           // without brackets for IPv6
    std::uint16_t port = 554;
    std::string pathAndQuery;   // starts with '/' (or empty)
    Credentials userinfo;       // percent-decoded; may be empty

    // "rtsp://host[:port]/path" WITHOUT credentials (what goes on the wire / in Digest uri).
    std::string requestUri() const;
};

// Only "rtsp://" is accepted (RTSPS/TLS is not implemented). nullopt on malformed input.
std::optional<RtspUrl> parseRtspUrl(std::string_view url);

// Replaces userinfo with "***" so the URL is safe to log.
std::string maskUrl(std::string_view url);

// Resolves an SDP a=control value against a base URL (RFC 2326 C.1.1).
std::string resolveControl(std::string_view base, std::string_view control);

}  // namespace hb::rtsp
