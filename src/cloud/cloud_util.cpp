#include "cloud/cloud_util.hpp"

#include "net/url.hpp"

#include <cctype>
#include <cstdlib>

namespace hb::cloud {

namespace {
std::string decodeEntities(std::string s) {
    struct E { const char* from; const char* to; };
    static const E es[] = {{"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&apos;", "'"}, {"&amp;", "&"}};
    for (const auto& e : es) {
        std::size_t pos = 0;
        const std::string from = e.from;
        while ((pos = s.find(from, pos)) != std::string::npos) {
            s.replace(pos, from.size(), e.to);
            pos += 1;
        }
    }
    return s;
}
}  // namespace

std::string xmlText(std::string_view xml, std::string_view tag) {
    const std::string open = "<" + std::string(tag) + ">";
    const std::string close = "</" + std::string(tag) + ">";
    const auto a = xml.find(open);
    if (a == std::string_view::npos) return {};
    const auto b = xml.find(close, a + open.size());
    if (b == std::string_view::npos) return {};
    return decodeEntities(std::string(xml.substr(a + open.size(), b - a - open.size())));
}

std::vector<std::string> xmlBlocks(std::string_view xml, std::string_view tag) {
    std::vector<std::string> out;
    const std::string open = "<" + std::string(tag) + ">";
    const std::string close = "</" + std::string(tag) + ">";
    std::size_t pos = 0;
    while (out.size() < 20000) {
        const auto a = xml.find(open, pos);
        if (a == std::string_view::npos) break;
        const auto b = xml.find(close, a + open.size());
        if (b == std::string_view::npos) break;
        out.emplace_back(xml.substr(a + open.size(), b - a - open.size()));
        pos = b + close.size();
    }
    return out;
}

std::string unquoteEtag(std::string s) {
    if (s.rfind("W/", 0) == 0) s.erase(0, 2);
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') s = s.substr(1, s.size() - 2);
    return s;
}

bool isHex32(const std::string& s) {
    if (s.size() != 32) return false;
    for (char c : s)
        if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    return true;
}

std::string sanitizeMessage(std::string_view s, std::size_t maxLen) {
    std::string o;
    for (unsigned char c : s) {
        if (c < 0x20 || c == 0x7F) continue;
        if (o.size() >= maxLen) {
            o += "...";
            break;
        }
        o += static_cast<char>(c);
    }
    return o;
}

std::string formEncode(std::string_view s) {
    static const char* hex = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            o += static_cast<char>(c);
        } else {
            o += '%';
            o += hex[c >> 4];
            o += hex[c & 15];
        }
    }
    return o;
}

Endpoint parseEndpoint(std::string_view text) {
    Endpoint e;
    std::string s(text);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(0, 1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    if (s.rfind("https://", 0) == 0) {
        e.scheme = "https";
        s.erase(0, 8);
    } else if (s.rfind("http://", 0) == 0) {
        e.scheme = "http";
        s.erase(0, 7);
    } else if (s.find("://") != std::string::npos) {
        return e;
    }
    while (!s.empty() && s.back() == '/') s.pop_back();
    if (s.empty() || s.size() > 253) return e;
    for (unsigned char c : s)
        if (std::isspace(c) || c == '/' || c == '@' || c == '?' || c == '#' || c == '\\' || c < 0x20) return e;
    const auto colon = s.rfind(':');
    if (colon != std::string::npos) {
        const std::string p = s.substr(colon + 1);
        if (p.empty() || p.size() > 5) return e;
        for (char c : p)
            if (!std::isdigit(static_cast<unsigned char>(c))) return e;
        const long port = std::strtol(p.c_str(), nullptr, 10);
        if (port < 1 || port > 65535) return e;
        e.port = static_cast<int>(port);
        s.erase(colon);
    }
    if (s.empty()) return e;
    for (unsigned char c : s)
        if (!(std::isalnum(c) || c == '-' || c == '.')) return e;
    if (e.scheme == "http") {
        // Cleartext (recordings + SigV4 headers) only toward the local network: loopback, RFC1918,
        // a single-label name (typical LAN MinIO/NAS) or a .local/.lan name. Anything else needs https.
        // Strict host checks live in src/net ("127.evil.com" / "10.0.0.1.evil.com" are not local).
        const bool local = hb::net::isLocalNetworkHost(s);
        if (!local) return e;
    }
    e.host = s;
    e.valid = true;
    return e;
}

}  // namespace hb::cloud
