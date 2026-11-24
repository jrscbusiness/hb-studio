#include "remote/http.hpp"

#include <algorithm>
#include <cstdint>

namespace hb::remote::http {

namespace {
bool isTokenChar(unsigned char c) {
    if (c <= 32 || c >= 127) return false;
    switch (c) {
        case '(': case ')': case '<': case '>': case '@': case ',': case ';': case ':': case '\\': case '"':
        case '/': case '[': case ']': case '?': case '=': case '{': case '}':
            return false;
        default:
            return true;
    }
}
std::string lower(std::string_view s) {
    std::string o(s);
    for (auto& c : o)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return o;
}
std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}
}  // namespace

RequestParser::State RequestParser::feed(const std::uint8_t* data, std::size_t len) {
    if (state_ != State::NeedMore) return state_;
    // Keep the head bounded: never hold more than max_ + one read worth of bytes.
    buf_.append(reinterpret_cast<const char*>(data), len);
    const auto end = buf_.find("\r\n\r\n");
    if (end == std::string::npos) {
        if (buf_.size() > max_) {
            status_ = 431;
            return state_ = State::Error;
        }
        return State::NeedMore;
    }
    if (end + 4 > max_) {
        status_ = 431;
        return state_ = State::Error;
    }
    leftover_ = buf_.substr(end + 4);
    state_ = parseHead(std::string_view(buf_).substr(0, end));
    return state_;
}

RequestParser::State RequestParser::parseHead(std::string_view head) {
    auto bad = [&](int st) { status_ = st; return State::Error; };
    for (unsigned char c : head)
        if (c == 0 || (c < 32 && c != '\r' && c != '\n' && c != '\t') || c == 127) return bad(400);
    std::size_t lineEnd = head.find("\r\n");
    std::string_view line = head.substr(0, lineEnd);
    // Request line: METHOD SP target SP HTTP/1.x
    const auto s1 = line.find(' ');
    if (s1 == std::string_view::npos) return bad(400);
    const auto s2 = line.find(' ', s1 + 1);
    if (s2 == std::string_view::npos) return bad(400);
    req_.method = std::string(line.substr(0, s1));
    req_.target = std::string(line.substr(s1 + 1, s2 - s1 - 1));
    const std::string_view ver = line.substr(s2 + 1);
    if (ver != "HTTP/1.1" && ver != "HTTP/1.0") return bad(505);
    if (req_.method.empty() || !std::all_of(req_.method.begin(), req_.method.end(), [](char c) { return isTokenChar(static_cast<unsigned char>(c)); }))
        return bad(400);
    if (req_.target.empty() || req_.target[0] != '/' || req_.target.size() > 2048) return bad(400);
    const auto q = req_.target.find('?');
    req_.path = req_.target.substr(0, q);
    std::size_t pos = lineEnd == std::string_view::npos ? head.size() : lineEnd + 2;
    int count = 0;
    while (pos < head.size()) {
        std::size_t e = head.find("\r\n", pos);
        if (e == std::string_view::npos) e = head.size();
        std::string_view h = head.substr(pos, e - pos);
        pos = e + 2;
        if (h.empty()) return bad(400);
        if (++count > 64) return bad(431);
        if (h.front() == ' ' || h.front() == '\t') return bad(400);  // obs-fold is forbidden
        const auto colon = h.find(':');
        if (colon == std::string_view::npos || colon == 0) return bad(400);
        std::string_view name = h.substr(0, colon);
        if (!std::all_of(name.begin(), name.end(), [](char c) { return isTokenChar(static_cast<unsigned char>(c)); })) return bad(400);
        std::string key = lower(name);
        if (req_.headers.count(key)) return bad(400);  // duplicates (request smuggling style tricks) rejected
        req_.headers[key] = std::string(trim(h.substr(colon + 1)));
    }
    // No request bodies are accepted at all.
    if (req_.headers.count("content-length") && req_.headers["content-length"] != "0") return bad(400);
    if (req_.headers.count("transfer-encoding")) return bad(400);
    return State::Done;
}

bool headerHasToken(std::string_view value, std::string_view token) {
    const std::string t = lower(token);
    std::size_t pos = 0;
    while (pos <= value.size()) {
        std::size_t e = value.find(',', pos);
        if (e == std::string_view::npos) e = value.size();
        if (lower(trim(value.substr(pos, e - pos))) == t) return true;
        pos = e + 1;
    }
    return false;
}

bool hostIsIpLiteralOrLocalhost(std::string_view h) {
    if (h.empty() || h.size() > 64) return false;
    std::string_view hostPart = h;
    if (h.front() == '[') {
        const auto rb = h.find(']');
        if (rb == std::string_view::npos) return false;
        hostPart = h.substr(1, rb - 1);
        const auto rest = h.substr(rb + 1);
        if (!rest.empty() && (rest[0] != ':' || rest.size() < 2)) return false;
        return !hostPart.empty() && std::all_of(hostPart.begin(), hostPart.end(), [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F') || c == ':' || c == '.';
        });
    }
    const auto colon = h.find(':');
    if (colon != std::string_view::npos) {
        const auto port = h.substr(colon + 1);
        if (port.empty() || port.size() > 5 || !std::all_of(port.begin(), port.end(), [](char c) { return c >= '0' && c <= '9'; })) return false;
        hostPart = h.substr(0, colon);
    }
    if (lower(hostPart) == "localhost") return true;
    // dotted quad
    int parts = 0;
    std::size_t i = 0;
    while (i <= hostPart.size()) {
        std::size_t e = hostPart.find('.', i);
        if (e == std::string_view::npos) e = hostPart.size();
        const auto seg = hostPart.substr(i, e - i);
        if (seg.empty() || seg.size() > 3 || !std::all_of(seg.begin(), seg.end(), [](char c) { return c >= '0' && c <= '9'; })) return false;
        if (std::stoi(std::string(seg)) > 255) return false;
        ++parts;
        i = e + 1;
    }
    return parts == 4;
}

bool originMatchesHost(std::string_view origin, std::string_view host) {
    static constexpr std::string_view kScheme = "http://";
    if (origin.size() <= kScheme.size() || lower(origin.substr(0, kScheme.size())) != kScheme) return false;
    return lower(origin.substr(kScheme.size())) == lower(host) && !host.empty();
}

const char* reason(int s) {
    switch (s) {
        case 101: return "Switching Protocols";
        case 200: return "OK";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 426: return "Upgrade Required";
        case 431: return "Request Header Fields Too Large";
        case 503: return "Service Unavailable";
        case 505: return "HTTP Version Not Supported";
        default: return "Error";
    }
}

std::string serialize(const Response& r) {
    std::string o = "HTTP/1.1 " + std::to_string(r.status) + " " + reason(r.status) + "\r\n";
    o += "Content-Type: " + r.contentType + "\r\n";
    o += "Content-Length: " + std::to_string(r.body.size()) + "\r\n";
    o += "Connection: close\r\n";
    o += "Cache-Control: no-store\r\n";
    o += "X-Content-Type-Options: nosniff\r\n";
    o += "X-Frame-Options: DENY\r\n";
    o += "Referrer-Policy: no-referrer\r\n";
    o += "Content-Security-Policy: default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; "
         "img-src blob: data:; connect-src 'self' ws:; base-uri 'none'; form-action 'none'; frame-ancestors 'none'\r\n";
    for (const auto& h : r.extraHeaders) o += h.first + ": " + h.second + "\r\n";
    o += "\r\n";
    o += r.body;
    return o;
}

}  // namespace hb::remote::http
