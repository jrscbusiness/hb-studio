#include "rtsp/url.hpp"

#include <cctype>

namespace hb::rtsp {

namespace {
bool istarts(std::string_view s, std::string_view p) {
    if (s.size() < p.size()) return false;
    for (std::size_t i = 0; i < p.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(s[i])) != p[i]) return false;
    return true;
}
int hexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
std::string pctDecode(std::string_view s) {
    std::string o;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && hexVal(s[i + 1]) >= 0 && hexVal(s[i + 2]) >= 0) {
            o += static_cast<char>(hexVal(s[i + 1]) * 16 + hexVal(s[i + 2]));
            i += 2;
        } else {
            o += s[i];
        }
    }
    return o;
}
}  // namespace

std::string RtspUrl::requestUri() const {
    std::string o = "rtsp://";
    if (host.find(':') != std::string::npos) o += "[" + host + "]";
    else o += host;
    if (port != 554) o += ":" + std::to_string(port);
    o += pathAndQuery;
    return o;
}

std::optional<RtspUrl> parseRtspUrl(std::string_view url) {
    if (!istarts(url, "rtsp://")) return std::nullopt;
    url.remove_prefix(7);
    auto slash = url.find_first_of("/?");
    std::string_view auth = url.substr(0, slash);
    RtspUrl r;
    if (slash != std::string_view::npos) {
        r.pathAndQuery = std::string(url.substr(slash));
        if (r.pathAndQuery[0] == '?') r.pathAndQuery.insert(0, "/");
    }
    auto at = auth.rfind('@');
    if (at != std::string_view::npos) {
        auto ui = auth.substr(0, at);
        auth.remove_prefix(at + 1);
        auto colon = ui.find(':');
        r.userinfo.username = pctDecode(ui.substr(0, colon));
        if (colon != std::string_view::npos) r.userinfo.password = pctDecode(ui.substr(colon + 1));
    }
    std::string_view hostPart = auth, portPart;
    if (!auth.empty() && auth[0] == '[') {
        auto close = auth.find(']');
        if (close == std::string_view::npos) return std::nullopt;
        hostPart = auth.substr(1, close - 1);
        auto rest = auth.substr(close + 1);
        if (!rest.empty()) {
            if (rest[0] != ':') return std::nullopt;
            portPart = rest.substr(1);
        }
    } else {
        auto colon = auth.rfind(':');
        if (colon != std::string_view::npos) {
            hostPart = auth.substr(0, colon);
            portPart = auth.substr(colon + 1);
        }
    }
    if (hostPart.empty()) return std::nullopt;
    r.host = std::string(hostPart);
    if (!portPart.empty()) {
        unsigned p = 0;
        for (char c : portPart) {
            if (c < '0' || c > '9') return std::nullopt;
            p = p * 10 + unsigned(c - '0');
            if (p > 65535) return std::nullopt;
        }
        if (p == 0) return std::nullopt;
        r.port = static_cast<std::uint16_t>(p);
    }
    return r;
}

std::string maskUrl(std::string_view url) {
    auto sch = url.find("://");
    if (sch == std::string_view::npos) return std::string(url);
    auto start = sch + 3;
    auto end = url.find_first_of("/?", start);
    auto auth = url.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
    auto at = auth.rfind('@');
    if (at == std::string_view::npos) return std::string(url);
    std::string o(url.substr(0, start));
    o += "***@";
    o += std::string(url.substr(start + at + 1));
    return o;
}

std::string resolveControl(std::string_view base, std::string_view control) {
    if (control.empty() || control == "*") return std::string(base);
    if (istarts(control, "rtsp://")) return std::string(control);
    std::string o(base);
    if (!o.empty() && o.back() != '/') o += '/';
    if (control[0] == '/') control.remove_prefix(1);
    o += std::string(control);
    return o;
}

}  // namespace hb::rtsp
