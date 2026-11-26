#include "net/url.hpp"

#include <cctype>
#include <cstdlib>

namespace hb::net {

std::string toLowerAscii(std::string_view s) {
    std::string o(s);
    for (auto& c : o)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return o;
}

bool parseDottedQuad(std::string_view h, int octets[4]) {
    int n = 0, value = 0, digits = 0;
    bool leadingZero = false;
    if (h.empty()) return false;
    for (std::size_t i = 0; i <= h.size(); ++i) {
        if (i == h.size() || h[i] == '.') {
            if (digits == 0 || value > 255 || n > 3) return false;
            octets[n++] = value;
            value = 0;
            digits = 0;
            leadingZero = false;
        } else if (h[i] >= '0' && h[i] <= '9') {
            if (leadingZero) return false;  // "010" / "00": ambiguous (octal) -> not a dotted quad
            if (digits == 0 && h[i] == '0') leadingZero = true;
            value = value * 10 + (h[i] - '0');
            if (++digits > 3) return false;
        } else {
            return false;
        }
    }
    return n == 4;
}

bool isLoopbackHost(std::string_view host) {
    const std::string h = toLowerAscii(host);
    if (h == "localhost" || h == "::1" || h == "[::1]") return true;
    int o[4];
    return parseDottedQuad(h, o) && o[0] == 127;
}

bool isRfc1918Host(std::string_view host) {
    int o[4];
    if (!parseDottedQuad(host, o)) return false;
    return o[0] == 10 || (o[0] == 192 && o[1] == 168) || (o[0] == 172 && o[1] >= 16 && o[1] <= 31);
}

bool isLocalNetworkHost(std::string_view host) {
    const std::string h = toLowerAscii(host);
    if (h.empty()) return false;
    if (h == "localhost" || h.find('.') == std::string::npos) return true;
    if (h.size() > 6 && h.compare(h.size() - 6, 6, ".local") == 0) return true;
    if (h.size() > 4 && h.compare(h.size() - 4, 4, ".lan") == 0) return true;
    return isLoopbackHost(h) || isRfc1918Host(h);
}

namespace {
bool fail(std::string* e, const char* msg) {
    if (e) *e = msg;
    return false;
}
}  // namespace

bool parseUrl(std::string_view text, Url& out, std::string* error) {
    out = Url{};
    for (unsigned char c : text)
        if (c <= 0x20 || c == 0x7F || c == '\\') return fail(error, "invalid character in URL");
    std::string_view rest = text;
    auto starts = [&](std::string_view p) {
        if (rest.size() < p.size()) return false;
        for (std::size_t i = 0; i < p.size(); ++i)
            if (std::tolower(static_cast<unsigned char>(rest[i])) != p[i]) return false;
        return true;
    };
    if (starts("https://")) { out.scheme = "https"; rest.remove_prefix(8); }
    else if (starts("http://")) { out.scheme = "http"; rest.remove_prefix(7); }
    else return fail(error, "unsupported scheme");
    const std::size_t frag = rest.find('#');
    if (frag != std::string_view::npos) rest = rest.substr(0, frag);
    const std::size_t pathStart = rest.find_first_of("/?");
    const std::string_view authority = rest.substr(0, pathStart);
    std::string target = pathStart == std::string_view::npos ? std::string("/") : std::string(rest.substr(pathStart));
    if (target[0] == '?') target = "/" + target;
    out.target = target;
    if (authority.empty() || authority.find('@') != std::string_view::npos) return fail(error, "missing host or user-info present");

    std::string_view hostPart, portPart;
    bool hasPortSep = false;
    if (authority[0] == '[') {
        const std::size_t close = authority.find(']');
        if (close == std::string_view::npos) return fail(error, "unterminated IPv6 literal");
        hostPart = authority.substr(1, close - 1);
        if (close + 1 < authority.size()) {
            if (authority[close + 1] != ':') return fail(error, "garbage after IPv6 literal");
            hasPortSep = true;
            portPart = authority.substr(close + 2);
        }
        if (hostPart.empty()) return fail(error, "empty host");
        for (unsigned char c : hostPart)
            if (!(std::isxdigit(c) || c == ':' || c == '.')) return fail(error, "invalid IPv6 literal");
    } else {
        const std::size_t colon = authority.rfind(':');
        hostPart = authority.substr(0, colon);
        if (colon != std::string_view::npos) {
            hasPortSep = true;
            portPart = authority.substr(colon + 1);
        }
        if (hostPart.empty()) return fail(error, "empty host");
        for (unsigned char c : hostPart)
            if (!(std::isalnum(c) || c == '.' || c == '-' || c == '_')) return fail(error, "invalid character in host");
    }
    out.port = out.https() ? 443 : 80;
    if (hasPortSep) {
        if (portPart.empty() || portPart.size() > 5) return fail(error, "port out of range");
        for (unsigned char c : portPart)
            if (!std::isdigit(c)) return fail(error, "port out of range");
        const long p = std::strtol(std::string(portPart).c_str(), nullptr, 10);
        if (p < 1 || p > 65535) return fail(error, "port out of range");
        out.port = static_cast<int>(p);
    }
    out.host = toLowerAscii(hostPart);
    return true;
}

}  // namespace hb::net
