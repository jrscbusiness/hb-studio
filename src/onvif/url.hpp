#pragma once
#include <string>
#include <string_view>

#include "onvif/result.hpp"

namespace hb::onvif {

struct Url {
    std::string scheme;    // lower-case: http, https, rtsp
    std::string host;      // without brackets for IPv6
    int port = 0;          // default for scheme when absent (80/443/554)
    std::string path;      // path + query, always starts with '/'
    std::string userinfo;  // never logged
};

Result<Url> parseUrl(std::string_view text);
std::string hostForUrl(const std::string& host);  // adds [] for IPv6
// scheme://host:port/path, WITHOUT userinfo (safe to log).
std::string urlToString(const Url& u);
// True for hosts a camera reports when it does not know its own address.
bool isUnroutableHost(std::string_view host);
// Replaces the host (keeping userinfo/port/path); returns the input unchanged if it cannot be parsed.
std::string replaceUrlHost(std::string_view url, std::string_view newHost);

}  // namespace hb::onvif
