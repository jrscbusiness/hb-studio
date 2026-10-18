#pragma once
#include <string>
#include <string_view>

namespace hb::core {

// Removes credentials from free text before it can reach a log sink:
// URL userinfo (rtsp://user:pass@host), password/token/passphrase style
// key=value pairs, Authorization headers, WS-Security <Password> elements
// and RTMP(S) stream keys.
std::string redactSecrets(std::string_view text);

}  // namespace hb::core
