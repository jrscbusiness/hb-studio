#include "chat/types.hpp"

#include <algorithm>
#include <cctype>

namespace hb::chat {

const char* platformKey(Platform p) {
    switch (p) {
        case Platform::YouTube: return "youtube";
        case Platform::Facebook: return "facebook";
        case Platform::Instagram: return "instagram";
        case Platform::Twitch: return "twitch";
    }
    return "twitch";
}
const char* platformLabel(Platform p) {
    switch (p) {
        case Platform::YouTube: return "YouTube";
        case Platform::Facebook: return "Facebook";
        case Platform::Instagram: return "Instagram";
        case Platform::Twitch: return "Twitch";
    }
    return "Twitch";
}
bool parsePlatform(std::string_view key, Platform& out) {
    for (int i = 0; i < kPlatformCount; ++i) {
        const auto p = static_cast<Platform>(i);
        if (key == platformKey(p)) { out = p; return true; }
    }
    return false;
}

const char* sourceStateName(SourceState s) {
    switch (s) {
        case SourceState::Disconnected: return "disconnected";
        case SourceState::Connecting: return "connecting";
        case SourceState::Connected: return "connected";
        case SourceState::Backoff: return "backoff";
        case SourceState::NeedsAuth: return "needs_auth";
        case SourceState::Failed: return "failed";
        case SourceState::Ended: return "ended";
        case SourceState::Unavailable: return "unavailable";
    }
    return "disconnected";
}

std::string sanitizeText(std::string_view in, std::size_t maxBytes) {
    std::string out;
    out.reserve(std::min(in.size(), maxBytes));
    std::size_t i = 0;
    while (i < in.size()) {
        const unsigned char c = static_cast<unsigned char>(in[i]);
        std::size_t len = 1;
        bool valid = true;
        if (c < 0x80) {
            len = 1;
        } else if (c >= 0xC2 && c <= 0xDF) {
            len = 2;
        } else if (c >= 0xE0 && c <= 0xEF) {
            len = 3;
        } else if (c >= 0xF0 && c <= 0xF4) {
            len = 4;
        } else {
            valid = false;
        }
        if (valid && len > 1) {
            if (i + len > in.size()) {
                valid = false;
            } else {
                for (std::size_t k = 1; k < len; ++k)
                    if ((static_cast<unsigned char>(in[i + k]) & 0xC0) != 0x80) { valid = false; break; }
            }
        }
        std::string piece;
        if (!valid) {
            piece = "?";
            len = 1;
        } else if (c < 0x20 || c == 0x7F) {
            piece = (c == '\n' || c == '\r' || c == '\t') ? " " : "";
        } else {
            piece.assign(in.substr(i, len));
        }
        if (out.size() + piece.size() > maxBytes) break;
        out += piece;
        i += len;
    }
    return out;
}

bool looksLikeCommand(std::string_view text) {
    std::size_t i = 0;
    while (i < text.size() && text[i] == ' ') ++i;
    return i + 1 < text.size() && text[i] == '!' && text[i + 1] != ' ';
}

bool isKnownBot(std::string_view author) {
    std::string a(author);
    std::transform(a.begin(), a.end(), a.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    static const char* const kBots[] = {"nightbot", "streamelements", "streamlabs", "moobot", "fossabot",
                                        "wizebot",  "stay_hydrated_bot", "commanderroot", "sery_bot"};
    for (const char* b : kBots)
        if (a == b) return true;
    return false;
}

std::string maskTail(const std::string& secret) {
    if (secret.empty()) return "";
    const std::size_t n = std::min<std::size_t>(4, secret.size());
    return "\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2" + secret.substr(secret.size() - n);
}

namespace {
bool digits(std::string_view s, std::size_t pos, std::size_t n, int& v) {
    if (pos + n > s.size()) return false;
    v = 0;
    for (std::size_t i = 0; i < n; ++i) {
        if (s[pos + i] < '0' || s[pos + i] > '9') return false;
        v = v * 10 + (s[pos + i] - '0');
    }
    return true;
}
// Days since 1970-01-01 for a proleptic Gregorian date (Howard Hinnant's algorithm).
std::int64_t daysFromCivil(std::int64_t y, int m, int d) {
    y -= m <= 2 ? 1 : 0;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const std::int64_t yoe = y - era * 400;
    const std::int64_t mp = m > 2 ? m - 3 : m + 9;
    const std::int64_t doy = (153 * mp + 2) / 5 + d - 1;
    const std::int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}
}  // namespace

bool parseIso8601Ms(std::string_view s, std::int64_t& out) {
    int Y, M, D, h, mi, sec;
    if (s.size() < 19 || !digits(s, 0, 4, Y) || s[4] != '-' || !digits(s, 5, 2, M) || s[7] != '-' ||
        !digits(s, 8, 2, D) || (s[10] != 'T' && s[10] != 't' && s[10] != ' ') || !digits(s, 11, 2, h) ||
        s[13] != ':' || !digits(s, 14, 2, mi) || s[16] != ':' || !digits(s, 17, 2, sec))
        return false;
    if (M < 1 || M > 12 || D < 1 || D > 31 || h > 23 || mi > 59 || sec > 60) return false;
    std::size_t p = 19;
    int ms = 0;
    if (p < s.size() && s[p] == '.') {
        ++p;
        int nd = 0;
        while (p < s.size() && s[p] >= '0' && s[p] <= '9') {
            if (nd < 3) { ms = ms * 10 + (s[p] - '0'); ++nd; }
            ++p;
        }
        if (nd == 0) return false;
        for (int k = nd; k < 3; ++k) ms *= 10;
    }
    std::int64_t offsetMin = 0;
    if (p < s.size()) {
        if (s[p] == 'Z' || s[p] == 'z') {
            ++p;
        } else if (s[p] == '+' || s[p] == '-') {
            const int sign = s[p] == '-' ? -1 : 1;
            ++p;
            int oh = 0, om = 0;
            if (!digits(s, p, 2, oh)) return false;
            p += 2;
            if (p < s.size() && s[p] == ':') ++p;
            if (p < s.size()) {
                if (!digits(s, p, 2, om)) return false;
                p += 2;
            }
            offsetMin = sign * (oh * 60 + om);
        } else {
            return false;
        }
    }
    if (p != s.size()) return false;
    const std::int64_t days = daysFromCivil(Y, M, D);
    out = ((days * 24 + h) * 60 + mi - offsetMin) * 60 * 1000 + static_cast<std::int64_t>(sec) * 1000 + ms;
    return true;
}

}  // namespace hb::chat
