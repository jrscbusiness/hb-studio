#include "core/redact.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <string_view>

// Linear-time redaction. (An earlier std::regex version was quadratic and could
// overflow the stack on long messages: see docs/REVIEW-1.md A3/A4.)
namespace hb::core {

namespace {

constexpr std::size_t kMaxLen = 8192;

inline char lc(char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
inline bool isWs(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
inline bool isIdent(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.';
}

std::string lower(std::string_view s) {
    std::string o(s);
    for (auto& c : o) c = lc(c);
    return o;
}

bool startsWithNoCase(std::string_view s, std::size_t pos, std::string_view w) {
    if (pos + w.size() > s.size()) return false;
    for (std::size_t i = 0; i < w.size(); ++i)
        if (lc(s[pos + i]) != w[i]) return false;
    return true;
}

bool endsWith(const std::string& s, std::string_view w) {
    return s.size() >= w.size() && s.compare(s.size() - w.size(), w.size(), w) == 0;
}

// ------------------------------------------------------------ URL userinfo
// scheme://user:pass@host  (password may itself contain '@' or '/').
std::string redactUserinfo(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    std::size_t i = 0;
    while (i < in.size()) {
        const std::size_t p = in.find("://", i);
        if (p == std::string::npos) break;
        // The scheme must be letters/digits/+-. ending right before "://".
        std::size_t s = p;
        while (s > i && (std::isalnum(static_cast<unsigned char>(in[s - 1])) || in[s - 1] == '+' || in[s - 1] == '-' || in[s - 1] == '.')) --s;
        const std::size_t start = p + 3;
        // The URL token ends at whitespace.
        std::size_t end = start;
        while (end < in.size() && !isWs(in[end])) ++end;
        const std::size_t lastAt = in.rfind('@', end == 0 ? 0 : end - 1);
        bool redact = false;
        std::size_t at = 0;
        if (s < p && lastAt != std::string::npos && lastAt >= start && lastAt < end) {
            const std::size_t firstSlash = in.find('/', start);
            if (firstSlash == std::string::npos || firstSlash > lastAt) {
                redact = true;  // ordinary userinfo
            } else {
                // A '/' precedes the last '@': userinfo only if "user:pass-part" (not host:port).
                const std::size_t colon = in.find(':', start);
                if (colon != std::string::npos && colon < firstSlash) {
                    bool allDigits = colon + 1 < firstSlash;
                    for (std::size_t k = colon + 1; k < firstSlash; ++k)
                        if (!std::isdigit(static_cast<unsigned char>(in[k]))) { allDigits = false; break; }
                    redact = !allDigits;
                }
            }
            at = lastAt;
        }
        out.append(in, i, start - i);
        if (redact) {
            const bool hasColon = in.find(':', start) < at;
            out += hasColon ? "***:***" : "***";
            out += '@';
            i = at + 1;
        } else {
            i = start;
        }
    }
    out.append(in, i, std::string::npos);
    return out;
}

// -------------------------------------------------- <...Password...>x</...>
std::string redactXmlPassword(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    std::size_t i = 0;
    while (i < in.size()) {
        const std::size_t lt = in.find('<', i);
        if (lt == std::string::npos) break;
        const std::size_t gt = in.find('>', lt);
        if (gt == std::string::npos) break;
        out.append(in, i, gt + 1 - i);
        i = gt + 1;
        if (in[lt + 1] == '/' ) continue;
        const std::string tag = lower(std::string_view(in).substr(lt, gt - lt));
        if (tag.find("password") == std::string::npos) continue;
        const std::size_t close = in.find('<', i);
        const std::size_t valEnd = close == std::string::npos ? in.size() : close;
        if (valEnd > i) out += "***";
        i = valEnd;
    }
    out.append(in, i, std::string::npos);
    return out;
}

// ------------------------------------------------------- rtmp(s) stream keys
std::string redactRtmpKeys(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    std::size_t i = 0;
    while (i < in.size()) {
        std::size_t p = std::string::npos;
        for (std::size_t k = i; k + 7 <= in.size(); ++k)
            if (startsWithNoCase(in, k, "rtmp://") || startsWithNoCase(in, k, "rtmps://")) { p = k; break; }
        if (p == std::string::npos) break;
        const std::size_t hostStart = in.find("://", p) + 3;
        std::size_t end = hostStart;
        while (end < in.size() && !isWs(in[end]) && in[end] != '?' && in[end] != '"' && in[end] != '\'') ++end;
        const std::size_t slash = in.rfind('/', end == 0 ? 0 : end - 1);
        out.append(in, i, (slash != std::string::npos && slash >= hostStart ? slash + 1 : end) - i);
        if (slash != std::string::npos && slash >= hostStart && slash + 1 < end) out += "***";
        i = end;
        if (!(slash != std::string::npos && slash >= hostStart)) { /* rtmp://host only: nothing to hide */ }
    }
    out.append(in, i, std::string::npos);
    return out;
}

// ---------------------------------------------------------------- key/value
bool isSecretKey(const std::string& id, char prev) {
    static const char* kStems[] = {"password", "passwd", "passphrase", "pwd", "token", "secret", "apikey", "api_key",
                                   "api-key", "streamkey", "stream_key", "stream-key", "sessionid", "session_id",
                                   "cookie", "authorization", "credential"};
    for (const char* s : kStems)
        if (id.find(s) != std::string::npos) return true;
    static const char* kExact[] = {"auth", "pass", "sig", "signature", "streamid"};
    for (const char* s : kExact)
        if (id == s) return true;
    if (id == "key" && (prev == '?' || prev == '&')) return true;
    return endsWith(id, "_key") || endsWith(id, "-key");
}

std::string redactKeyValues(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    std::size_t i = 0;
    while (i < in.size()) {
        if (!isIdent(in[i])) { out += in[i++]; continue; }
        const std::size_t b = i;
        while (i < in.size() && isIdent(in[i])) ++i;
        out.append(in, b, i - b);
        const std::string id = lower(std::string_view(in).substr(b, i - b));
        const char prev = b > 0 ? in[b - 1] : '\0';
        if (!isSecretKey(id, prev)) continue;

        std::size_t j = i;
        if (j < in.size() && (in[j] == '"' || in[j] == '\'')) ++j;  // JSON / quoted key
        while (j < in.size() && (in[j] == ' ' || in[j] == '\t')) ++j;
        if (j >= in.size() || (in[j] != ':' && in[j] != '=')) continue;
        const char sep = in[j++];
        while (j < in.size() && (in[j] == ' ' || in[j] == '\t')) ++j;
        if (j >= in.size()) continue;

        out.append(in, i, j - i);  // keep key, quote, separator, spaces
        std::size_t e = j;
        if (in[j] == '"' || in[j] == '\'') {
            const char q = in[j];
            e = j + 1;
            while (e < in.size() && in[e] != q) e += (in[e] == '\\' && q == '"' && e + 1 < in.size()) ? 2 : 1;
            out += q;
            out += "***";
            if (e < in.size()) out += q;
            i = std::min(e + 1, in.size());
        } else if (sep == ':') {
            while (e < in.size() && in[e] != '\n' && in[e] != '\r') ++e;  // header style: to end of line
            out += "***";
            i = e;
        } else {
            while (e < in.size() && !isWs(in[e]) && in[e] != '&') ++e;     // key=value[&...]
            out += "***";
            i = e;
        }
    }
    return out;
}

// Bare "Bearer <token>" / "Basic <base64>" credentials.
std::string redactBareCredentials(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    std::size_t i = 0;
    while (i < in.size()) {
        const bool boundary = i == 0 || !isIdent(in[i - 1]);
        std::size_t kwLen = 0;
        if (boundary && startsWithNoCase(in, i, "bearer ")) kwLen = 7;
        else if (boundary && startsWithNoCase(in, i, "basic ")) kwLen = 6;
        if (!kwLen) { out += in[i++]; continue; }
        std::size_t e = i + kwLen;
        while (e < in.size() && (std::isalnum(static_cast<unsigned char>(in[e])) || std::strchr("+/=._~-", in[e]) != nullptr)) ++e;
        if (e - (i + kwLen) >= 12) {
            out.append(in, i, kwLen);
            out += "***";
            i = e;
        } else {
            out += in[i++];
        }
    }
    return out;
}

}  // namespace

std::string redactSecrets(std::string_view text) {
    std::string s(text.substr(0, kMaxLen));
    const bool truncated = text.size() > kMaxLen;
    s = redactUserinfo(s);
    s = redactXmlPassword(s);
    s = redactRtmpKeys(s);
    s = redactKeyValues(s);
    s = redactBareCredentials(s);
    if (truncated) s += "...[truncado]";
    return s;
}

}  // namespace hb::core
