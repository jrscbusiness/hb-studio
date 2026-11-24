#pragma once
// Tiny helpers shared by the providers: lenient XML text extraction (S3 responses are small and
// flat), URL form encoding, endpoint parsing, message sanitising.
#include <string>
#include <string_view>
#include <vector>

namespace hb::cloud {

// Text of the first <tag>...</tag> in `xml` with XML entities decoded; "" if absent.
std::string xmlText(std::string_view xml, std::string_view tag);
// Inner XML of every <tag>...</tag> block (non-nested use only), in order.
std::vector<std::string> xmlBlocks(std::string_view xml, std::string_view tag);

std::string unquoteEtag(std::string s);  // strips surrounding quotes and a leading W/
bool isHex32(const std::string& s);      // looks like a hex MD5

// Remote-supplied text made safe for the UI/log: control chars dropped, truncated.
std::string sanitizeMessage(std::string_view s, std::size_t maxLen = 200);

// application/x-www-form-urlencoded value encoding (space -> '+' is NOT used: %20).
std::string formEncode(std::string_view s);

struct Endpoint {
    bool valid = false;
    std::string scheme = "https";
    std::string host;
    int port = 0;  // 0 = scheme default
};
// Accepts "host", "https://host", "http://host:9000", optional trailing '/'. Rejects paths,
// userinfo, whitespace and empty hosts.
Endpoint parseEndpoint(std::string_view text);

}  // namespace hb::cloud
