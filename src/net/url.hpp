#pragma once
// Shared URL parsing and host classification for every network-facing module. The host checks are
// deliberately strict: "127.evil.com" or "10.0.0.1.evil.com" must never be mistaken for a local
// address (that would send keys/recordings in clear to a remote host), and octets with leading zeros
// ("010.0.0.1", read as octal by some resolvers) are not treated as dotted quads at all.
#include <string>
#include <string_view>

namespace hb::net {

struct Url {
    std::string scheme;  // "http" or "https", lower case
    std::string host;    // lower case, IPv6 without brackets
    int port = 0;        // explicit port or the scheme default (80/443)
    std::string target;  // path + query, always starts with '/'; the fragment is dropped
    bool https() const { return scheme == "https"; }
};

// Accepts only http/https URLs with a non-empty host made of [A-Za-z0-9._-] (or a bracketed IPv6
// literal), an optional port 1-65535 and no user-info, whitespace, control characters or backslashes.
// On failure returns false and (optionally) a short English reason.
bool parseUrl(std::string_view text, Url& out, std::string* error = nullptr);

// Parses "a.b.c.d" (four decimal octets 0-255, no leading zeros, nothing else). Returns false otherwise.
bool parseDottedQuad(std::string_view host, int octets[4]);

std::string toLowerAscii(std::string_view s);

// localhost, ::1 (also "[::1]") and 127.x.x.x written as a strict dotted quad.
bool isLoopbackHost(std::string_view host);
// 10/8, 172.16/12, 192.168/16 as a strict dotted quad.
bool isRfc1918Host(std::string_view host);
// Where cleartext HTTP is acceptable on a LAN: loopback, RFC1918, a single-label name (typical NAS),
// or a .local/.lan name.
bool isLocalNetworkHost(std::string_view host);

}  // namespace hb::net
