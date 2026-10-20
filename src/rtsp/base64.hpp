#pragma once
#include <optional>
#include <string>
#include <string_view>

#include "rtsp/types.hpp"

namespace hb::rtsp {

std::string base64Encode(ByteView data);
// Tolerant: ignores whitespace, accepts missing padding. nullopt on invalid characters.
std::optional<Bytes> base64Decode(std::string_view text);

}  // namespace hb::rtsp
