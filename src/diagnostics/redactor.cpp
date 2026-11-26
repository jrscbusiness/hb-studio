#include "diagnostics/redactor.hpp"

#include <algorithm>
#include <cctype>

#include "core/redact.hpp"

namespace hb::diag {

namespace {

inline bool isAlnum(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0; }
inline bool isDigit(char c) { return c >= '0' && c <= '9'; }
inline bool isHex(char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; }
inline char lc(char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
inline char norm(char c) { return c == '/' ? '\\' : lc(c); }

void replaceAll(std::string& s, const std::string& from, const std::string& to) {
    if (from.empty()) return;
    std::size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
}

std::string percentEncode(const std::string& v) {
    static const char* hex = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : v) {
        if (isAlnum(static_cast<char>(c)) || c == '-' || c == '_' || c == '.' || c == '~') o += static_cast<char>(c);
        else { o += '%'; o += hex[c >> 4]; o += hex[c & 15]; }
    }
    return o;
}

std::string jsonEsc(const std::string& v) {
    std::string o;
    for (char c : v) {
        if (c == '"' || c == '\\') o += '\\';
        o += c;
    }
    return o;
}

// Case-insensitive, slash-insensitive replacement of `fromNorm` (already normalised) by `to`.
std::string replacePathCI(const std::string& text, const std::string& fromNorm, const std::string& to) {
    if (fromNorm.empty()) return text;
    std::string out;
    out.reserve(text.size());
    std::size_t i = 0;
    while (i < text.size()) {
        bool match = i + fromNorm.size() <= text.size();
        for (std::size_t k = 0; match && k < fromNorm.size(); ++k)
            if (norm(text[i + k]) != fromNorm[k]) match = false;
        if (match) { out += to; i += fromNorm.size(); }
        else out += text[i++];
    }
    return out;
}

bool startsWithCI(std::string_view s, std::size_t pos, std::string_view w) {
    if (pos + w.size() > s.size()) return false;
    for (std::size_t i = 0; i < w.size(); ++i)
        if (norm(s[pos + i]) != norm(w[i])) return false;
    return true;
}

bool isNameEnd(char c) {
    return c == '\\' || c == '/' || c == ' ' || c == '"' || c == '\'' || c == '\n' || c == '\r' || c == '\t' || c == ',' || c == ';';
}

// True if `run` ("a.b.c.d" or "a.b.c.d:port") is a dotted-quad IPv4 address.
bool isIpv4(std::string_view run) {
    const std::size_t colon = run.find(':');
    std::string_view ip = run.substr(0, colon);
    if (colon != std::string_view::npos) {
        const std::string_view port = run.substr(colon + 1);
        if (port.empty() || port.size() > 5 || !std::all_of(port.begin(), port.end(), isDigit)) return false;
    }
    int groups = 0;
    std::size_t p = 0;
    while (p <= ip.size()) {
        std::size_t q = p;
        while (q < ip.size() && ip[q] != '.') ++q;
        const std::string_view g = ip.substr(p, q - p);
        if (g.empty() || g.size() > 3 || !std::all_of(g.begin(), g.end(), isDigit)) return false;
        int val = 0;
        for (char ch : g) val = val * 10 + (ch - '0');
        if (val > 255) return false;
        ++groups;
        p = q + 1;
    }
    return groups == 4 && ip != "127.0.0.1" && ip != "0.0.0.0";
}

}  // namespace

void Redactor::addSecret(std::string value) {
    if (value.size() < kMinSecretLen) return;
    secrets_.push_back(std::move(value));
    // Longest first so a secret that contains another one is removed whole.
    std::stable_sort(secrets_.begin(), secrets_.end(), [](const auto& a, const auto& b) { return a.size() > b.size(); });
}

void Redactor::addPathAlias(std::string path, std::string alias) {
    while (!path.empty() && (path.back() == '\\' || path.back() == '/')) path.pop_back();
    if (path.size() < 3) return;
    std::string n;
    for (char c : path) n += norm(c);
    aliases_.emplace_back(std::move(n), std::move(alias));
    std::stable_sort(aliases_.begin(), aliases_.end(), [](const auto& a, const auto& b) { return a.first.size() > b.first.size(); });
}

std::string redactUserPaths(std::string_view text) {
    // "<drive>:\Users\<name>" / "/home/<name>" / "/Users/<name>": the <name> segment becomes "<user>".
    static const char* const kRoots[] = {"\\users\\", "/home/", "/users/"};
    std::string out;
    out.reserve(text.size());
    std::size_t i = 0;
    while (i < text.size()) {
        std::size_t matched = 0;
        for (const char* r : kRoots) {
            const std::string_view rv(r);
            if (startsWithCI(text, i, rv)) { matched = rv.size(); break; }
        }
        if (matched == 0) { out += text[i++]; continue; }
        out.append(text.substr(i, matched));
        i += matched;
        std::size_t j = i;
        while (j < text.size() && !isNameEnd(text[j])) ++j;
        if (j > i) out += "<user>";
        i = j;
    }
    return out;
}

std::string redactNetworkIds(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    const std::size_t n = text.size();
    std::size_t i = 0;
    while (i < n) {
        const char c = text[i];
        // ---- e-mail: the '@' is the anchor; walk back over the local part already copied to `out`.
        if (c == '@') {
            std::size_t ls = out.size();
            while (ls > 0 && (isAlnum(out[ls - 1]) || out[ls - 1] == '.' || out[ls - 1] == '_' || out[ls - 1] == '%' ||
                              out[ls - 1] == '+' || out[ls - 1] == '-'))
                --ls;
            std::size_t j = i + 1;
            while (j < n && (isAlnum(text[j]) || text[j] == '.' || text[j] == '-')) ++j;
            const std::string_view dom = text.substr(i + 1, j - i - 1);
            const std::size_t dot = dom.rfind('.');
            if (ls < out.size() && dot != std::string_view::npos && dot > 0 && dom.size() - dot - 1 >= 2) {
                out.resize(ls);
                out += "<email>";
                i = j;
                continue;
            }
        }
        // ---- IPv4 / IPv6 / MAC: maximal run of [0-9a-fA-F:.] that starts at a word boundary.
        if ((isHex(c) || c == ':') && (i == 0 || !isAlnum(text[i - 1]))) {
            std::size_t j = i;
            while (j < n && (isHex(text[j]) || text[j] == ':' || text[j] == '.')) ++j;
            std::size_t e = j;
            while (e > i && (text[e - 1] == '.' || text[e - 1] == ':')) --e;   // trailing punctuation is not part of it
            const bool glued = j < n && isAlnum(text[j]);                     // run continues into a word
            const std::string_view run = text.substr(i, e - i);
            const char* repl = nullptr;
            if (!glued && !run.empty()) {
                const auto colons = std::count(run.begin(), run.end(), ':');
                const auto dots = std::count(run.begin(), run.end(), '.');
                if (dots == 3 && colons <= 1) {
                    if (isIpv4(run)) repl = "<ip>";
                } else if (dots == 0 && colons >= 2 && (colons >= 5 || run.find("::") != std::string_view::npos)) {
                    repl = (colons == 5 && run.size() == 17) ? "<mac>" : "<ip>";   // a hh:mm:ss clock has 2 colons, no "::"
                }
            }
            if (repl) {
                out += repl;
                i = e;
            } else {
                out.append(text.substr(i, j - i));
                i = j;
            }
            continue;
        }
        out += c;
        ++i;
    }
    return out;
}

std::string redactLongTokens(std::string_view text) {
    // 32+ chars of [A-Za-z0-9_-] with at least one letter and one digit: API keys, JWT parts, hashes.
    std::string out;
    out.reserve(text.size());
    std::size_t i = 0;
    const std::size_t n = text.size();
    auto tokChar = [](char c) { return isAlnum(c) || c == '_' || c == '-'; };
    while (i < n) {
        if (!tokChar(text[i])) { out += text[i++]; continue; }
        std::size_t j = i;
        bool letter = false, digit = false;
        while (j < n && tokChar(text[j])) {
            letter = letter || std::isalpha(static_cast<unsigned char>(text[j])) != 0;
            digit = digit || isDigit(text[j]);
            ++j;
        }
        if (j - i >= 32 && letter && digit) out += "<token>";
        else out.append(text.substr(i, j - i));
        i = j;
    }
    return out;
}

std::string Redactor::redact(std::string_view text) const {
    std::string s(text);
    for (const auto& sec : secrets_) {
        replaceAll(s, sec, "<segredo>");
        replaceAll(s, percentEncode(sec), "<segredo>");
        replaceAll(s, jsonEsc(sec), "<segredo>");
    }
    // hb::core::redactSecrets truncates at 8 KiB: apply it per line (and per 4000-byte chunk of very
    // long lines) so a long log is covered entirely.
    std::string r;
    r.reserve(s.size());
    std::size_t p = 0;
    constexpr std::size_t kChunk = 4000;
    while (p <= s.size()) {
        std::size_t e = s.find('\n', p);
        if (e == std::string::npos) e = s.size();
        std::string_view line(s.data() + p, e - p);
        while (line.size() > kChunk) {
            r += core::redactSecrets(line.substr(0, kChunk));
            line.remove_prefix(kChunk);
        }
        r += core::redactSecrets(line);
        if (e < s.size()) r += '\n';
        p = e + 1;
    }
    for (const auto& a : aliases_) r = replacePathCI(r, a.first, a.second);
    r = redactUserPaths(r);
    r = redactNetworkIds(r);
    r = redactLongTokens(r);
    return r;
}

std::string urlSchemeHost(std::string_view url) {
    const std::size_t p = url.find("://");
    if (p == std::string_view::npos || p == 0 || p > 12) return {};
    for (std::size_t k = 0; k < p; ++k)
        if (!isAlnum(url[k]) && url[k] != '+' && url[k] != '-' && url[k] != '.') return {};
    const std::size_t s = p + 3;
    std::size_t e = s;
    while (e < url.size() && url[e] != '/' && url[e] != '?' && url[e] != '#' && url[e] != ' ') ++e;
    std::string_view auth = url.substr(s, e - s);
    const std::size_t at = auth.rfind('@');
    if (at != std::string_view::npos) auth.remove_prefix(at + 1);
    if (auth.empty()) return {};
    return std::string(url.substr(0, p + 3)) + std::string(auth);
}

}  // namespace hb::diag
