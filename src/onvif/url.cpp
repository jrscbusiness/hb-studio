#include "onvif/url.hpp"

#include <algorithm>
#include <cctype>

namespace hb::onvif {

Result<Url> parseUrl(std::string_view text) {
    Url u;
    const auto sep = text.find("://");
    if (sep == std::string_view::npos || sep == 0) return makeError(ErrorCode::InvalidArgument, "URL without scheme");
    u.scheme = std::string(text.substr(0, sep));
    std::transform(u.scheme.begin(), u.scheme.end(), u.scheme.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::string_view rest = text.substr(sep + 3);
    const auto slash = rest.find_first_of("/?#");
    std::string_view authority = rest.substr(0, slash);
    std::string_view path = slash == std::string_view::npos ? std::string_view{} : rest.substr(slash);
    if (!path.empty() && path[0] != '/') path = {};  // "?query" / "#frag" without path: treat as "/"
    const auto frag = path.find('#');
    if (frag != std::string_view::npos) path = path.substr(0, frag);
    u.path = path.empty() ? "/" : std::string(path);
    if (const auto at = authority.rfind('@'); at != std::string_view::npos) {
        u.userinfo = std::string(authority.substr(0, at));
        authority = authority.substr(at + 1);
    }
    std::string_view hostPart = authority, portPart;
    if (!authority.empty() && authority[0] == '[') {
        const auto close = authority.find(']');
        if (close == std::string_view::npos) return makeError(ErrorCode::InvalidArgument, "bad IPv6 literal in URL");
        hostPart = authority.substr(1, close - 1);
        if (close + 1 < authority.size()) {
            if (authority[close + 1] != ':') return makeError(ErrorCode::InvalidArgument, "bad authority in URL");
            portPart = authority.substr(close + 2);
        }
    } else if (const auto colon = authority.rfind(':'); colon != std::string_view::npos) {
        hostPart = authority.substr(0, colon);
        portPart = authority.substr(colon + 1);
    }
    if (hostPart.empty()) return makeError(ErrorCode::InvalidArgument, "URL without host");
    u.host = std::string(hostPart);
    if (!portPart.empty()) {
        int p = 0;
        for (char c : portPart) {
            if (c < '0' || c > '9' || p > 65535) return makeError(ErrorCode::InvalidArgument, "bad port in URL");
            p = p * 10 + (c - '0');
        }
        if (p < 1 || p > 65535) return makeError(ErrorCode::InvalidArgument, "bad port in URL");
        u.port = p;
    } else {
        u.port = u.scheme == "https" ? 443 : u.scheme == "rtsp" ? 554 : 80;
    }
    return u;
}

std::string hostForUrl(const std::string& host) {
    return host.find(':') != std::string::npos ? "[" + host + "]" : host;
}

std::string urlToString(const Url& u) {
    return u.scheme + "://" + hostForUrl(u.host) + ":" + std::to_string(u.port) + u.path;
}

bool isUnroutableHost(std::string_view host) {
    return host == "0.0.0.0" || host == "localhost" || host == "::" || host == "::1" || host.substr(0, 4) == "127.";
}

std::string replaceUrlHost(std::string_view url, std::string_view newHost) {
    const auto sep = url.find("://");
    if (sep == std::string_view::npos) return std::string(url);
    const auto authStart = sep + 3;
    const auto authEnd = url.find_first_of("/?#", authStart);
    std::string_view authority =
        url.substr(authStart, authEnd == std::string_view::npos ? std::string_view::npos : authEnd - authStart);
    std::string userinfo;
    if (const auto at = authority.rfind('@'); at != std::string_view::npos) {
        userinfo = std::string(authority.substr(0, at + 1));
        authority = authority.substr(at + 1);
    }
    std::string port;
    if (!authority.empty() && authority[0] == '[') {
        if (const auto c = authority.find("]:"); c != std::string_view::npos) port = std::string(authority.substr(c + 1));
    } else if (const auto c = authority.rfind(':'); c != std::string_view::npos) {
        port = std::string(authority.substr(c));
    }
    std::string out(url.substr(0, authStart));
    out += userinfo;
    out += hostForUrl(std::string(newHost));
    out += port;
    if (authEnd != std::string_view::npos) out += std::string(url.substr(authEnd));
    return out;
}

}  // namespace hb::onvif
