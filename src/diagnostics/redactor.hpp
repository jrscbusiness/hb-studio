#pragma once
// Redaction for diagnostic exports. Defense in depth on top of hb::core::redactSecrets:
//   1. exact secret values registered by the caller (stream keys, API keys, tokens...) -> "<segredo>"
//   2. hb::core::redactSecrets (URL userinfo, password=..., Authorization, RTMP keys...), line by line
//   3. user-name paths: registered aliases (%LOCALAPPDATA%, %USERPROFILE%) and any C:\Users\<name>,
//      /home/<name>, /Users/<name> -> "<user>"
//   4. e-mail addresses, IPv4/IPv6/MAC addresses (remote devices) and long opaque tokens
// Portable, linear-time, never throws on hostile input.
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace hb::diag {

class Redactor {
public:
    // Exact value to remove wherever it appears (also its percent-encoded and JSON-escaped forms).
    // Values shorter than kMinSecretLen are ignored: they would mangle ordinary text.
    void addSecret(std::string value);
    // Replaces every (case-insensitive, '/' == '\\') occurrence of `path` by `alias`.
    void addPathAlias(std::string path, std::string alias);

    std::string redact(std::string_view text) const;

    static constexpr std::size_t kMinSecretLen = 4;

private:
    std::vector<std::string> secrets_;
    std::vector<std::pair<std::string, std::string>> aliases_;
};

// Individual passes (exposed for tests).
std::string redactUserPaths(std::string_view text);
std::string redactNetworkIds(std::string_view text);   // e-mails, IPv4, IPv6, MAC
std::string redactLongTokens(std::string_view text);

// "rtmps://user:pw@host:443/app/KEY?x=1" -> "rtmps://host:443" (scheme + host only); "" if not a URL.
std::string urlSchemeHost(std::string_view url);

}  // namespace hb::diag
