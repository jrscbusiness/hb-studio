#pragma once
#include <array>
#include <string>
#include <string_view>

#include "rtsp/types.hpp"

namespace hb::rtsp {

// RFC 1321 MD5 (needed for RTSP Digest authentication; not used for anything security critical).
std::array<std::uint8_t, 16> md5(ByteView data);
std::string md5Hex(std::string_view text);

}  // namespace hb::rtsp
