#include "captions/text_normalizer.hpp"

#include <algorithm>
#include <cstdint>

namespace hb::captions {
namespace {

std::size_t seqLen(unsigned char c) {
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

// Length of the UTF-8 sequence starting at s[i], clamped to a valid, complete one (else 1 byte).
std::size_t charLen(std::string_view s, std::size_t i) {
    const std::size_t n = seqLen(static_cast<unsigned char>(s[i]));
    if (n == 1 || i + n > s.size()) return 1;
    for (std::size_t k = 1; k < n; ++k)
        if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return 1;
    return n;
}

bool isSpace(unsigned char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f'; }

// Uppercases the character at s[i] in place when it is ASCII a-z or Latin-1 lowercase (U+00E0..U+00FE).
void upperAt(std::string& s, std::size_t i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (c >= 'a' && c <= 'z') { s[i] = static_cast<char>(c - 32); return; }
    if (c == 0xC3 && i + 1 < s.size()) {
        const unsigned char d = static_cast<unsigned char>(s[i + 1]);
        if (d >= 0xA0 && d <= 0xBE && d != 0xB7) s[i + 1] = static_cast<char>(d - 0x20);
    }
}

bool isAlphaAt(std::string_view s, std::size_t i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) return true;
    if (c == 0xC3 && i + 1 < s.size()) {
        const unsigned char d = static_cast<unsigned char>(s[i + 1]);
        return (d >= 0x80 && d <= 0xBF && d != 0x97 && d != 0xB7);
    }
    return false;
}

// Lower-cases ASCII and Latin-1 capitals; used only for matching.
std::string foldLower(std::string_view w) {
    std::string out;
    out.reserve(w.size());
    for (std::size_t i = 0; i < w.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(w[i]);
        if (c >= 'A' && c <= 'Z') {
            out.push_back(static_cast<char>(c + 32));
        } else if (c == 0xC3 && i + 1 < w.size()) {
            unsigned char d = static_cast<unsigned char>(w[i + 1]);
            if (d >= 0x80 && d <= 0x9E && d != 0x97) d = static_cast<unsigned char>(d + 0x20);
            out.push_back(static_cast<char>(c));
            out.push_back(static_cast<char>(d));
            ++i;
        } else {
            out.push_back(static_cast<char>(c));
        }
    }
    return out;
}

// Deliberately small list (pt-BR first, a few English words). Whole-word, case-insensitive.
const char* const kProfanity[] = {
    "porra", "caralho", "merda", "puta", "puto", "buceta", "foda", "foder", "fodido", "fodida", "foda-se",
    "cacete", "bosta", "arrombado", "arrombada", "viado", "otário", "otária", "babaca", "cuzão", "fdp",
    "fuck", "fucking", "shit", "bitch", "asshole", "bastard"};

bool isProfane(const std::string& folded) {
    for (const char* p : kProfanity)
        if (folded == p) return true;
    return false;
}

std::string maskWord(const std::string& core) {
    // keep the first code point, replace the rest with '*'
    const std::size_t first = charLen(core, 0);
    std::string out = core.substr(0, first);
    for (std::size_t i = first; i < core.size(); i += charLen(core, i)) out.push_back('*');
    return out;
}

std::string maskProfanity(std::string_view text) {
    std::string out;
    auto punct = [](unsigned char c) {
        return c < 0x80 && !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-');
    };
    for (const std::string& token : splitWords(text)) {
        std::size_t b = 0, e = token.size();
        while (b < e && punct(static_cast<unsigned char>(token[b]))) ++b;
        while (e > b && punct(static_cast<unsigned char>(token[e - 1]))) --e;
        std::string core = token.substr(b, e - b);
        if (!core.empty() && isProfane(foldLower(core))) core = maskWord(core);
        if (!out.empty()) out.push_back(' ');
        out += token.substr(0, b) + core + token.substr(e);
    }
    return out;
}

}  // namespace

std::size_t utf8Length(std::string_view s) {
    std::size_t n = 0;
    for (std::size_t i = 0; i < s.size(); i += charLen(s, i)) ++n;
    return n;
}

std::vector<std::string> splitWords(std::string_view s) {
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && isSpace(static_cast<unsigned char>(s[i]))) ++i;
        std::size_t j = i;
        while (j < s.size() && !isSpace(static_cast<unsigned char>(s[j]))) ++j;
        if (j > i) out.emplace_back(s.substr(i, j - i));
        i = j;
    }
    return out;
}

std::string cleanText(std::string_view text) {
    std::string stripped;
    stripped.reserve(text.size());
    int depth = 0;
    for (std::size_t i = 0; i < text.size();) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c == '[' || c == '(') { ++depth; ++i; continue; }
        if ((c == ']' || c == ')') && depth > 0) { --depth; stripped.push_back(' '); ++i; continue; }
        if (depth > 0) { ++i; continue; }
        // music notes U+266A / U+266B: E2 99 AA / E2 99 AB
        if (c == 0xE2 && i + 2 < text.size() && static_cast<unsigned char>(text[i + 1]) == 0x99 &&
            (static_cast<unsigned char>(text[i + 2]) == 0xAA || static_cast<unsigned char>(text[i + 2]) == 0xAB)) {
            stripped.push_back(' ');
            i += 3;
            continue;
        }
        if (c < 0x20 && !isSpace(c)) { ++i; continue; }
        stripped.push_back(static_cast<char>(c));
        ++i;
    }
    std::string out;
    for (const std::string& w : splitWords(stripped)) {
        if (!out.empty()) out.push_back(' ');
        out += w;
    }
    return out;
}

std::string normalizeCaption(std::string_view text, const NormalizeOptions& opt, bool isFinal) {
    std::string t = cleanText(text);
    if (t.empty()) return t;
    if (opt.profanityFilter) t = maskProfanity(t);
    if (opt.capitalize) {
        bool atStart = true;
        for (std::size_t i = 0; i < t.size();) {
            const std::size_t n = charLen(t, i);
            if (atStart && isAlphaAt(t, i)) {
                upperAt(t, i);
                atStart = false;
            } else if (atStart && t[i] >= '0' && t[i] <= '9') {
                atStart = false;
            }
            if (n == 1 && (t[i] == '.' || t[i] == '!' || t[i] == '?')) atStart = true;
            i += n;
        }
    }
    if (isFinal && opt.finalPunctuation) {
        const char last = t.back();
        const bool ellipsis = t.size() >= 3 && t.compare(t.size() - 3, 3, "\xE2\x80\xA6") == 0;
        const bool quotedEnd = (last == '"' || last == '\'') && t.size() >= 2 &&
                               (t[t.size() - 2] == '.' || t[t.size() - 2] == '!' || t[t.size() - 2] == '?');
        if (!(last == '.' || last == '!' || last == '?' || ellipsis || quotedEnd)) t.push_back('.');
    }
    return t;
}

std::vector<std::string> wrapLines(std::string_view text, std::size_t maxChars) {
    std::vector<std::string> lines;
    if (maxChars == 0) maxChars = 1;
    std::string cur;
    std::size_t curLen = 0;
    auto flush = [&] {
        if (!cur.empty()) { lines.push_back(cur); cur.clear(); curLen = 0; }
    };
    for (const std::string& w : splitWords(text)) {
        std::string word = w;
        std::size_t wl = utf8Length(word);
        while (wl > maxChars) {   // hard split by code points
            flush();
            std::size_t bytes = 0, cps = 0;
            while (bytes < word.size() && cps < maxChars) { bytes += charLen(word, bytes); ++cps; }
            lines.push_back(word.substr(0, bytes));
            word.erase(0, bytes);
            wl -= cps;
        }
        if (word.empty()) continue;
        if (curLen == 0) { cur = word; curLen = wl; }
        else if (curLen + 1 + wl <= maxChars) { cur += ' '; cur += word; curLen += 1 + wl; }
        else { flush(); cur = word; curLen = wl; }
    }
    flush();
    return lines;
}

std::vector<std::string> paginate(std::string_view text, std::size_t maxChars, std::size_t maxLines) {
    if (maxLines == 0) maxLines = 1;
    std::vector<std::string> pages;
    const auto lines = wrapLines(text, maxChars);
    for (std::size_t i = 0; i < lines.size(); i += maxLines) {
        std::string page;
        for (std::size_t k = i; k < lines.size() && k < i + maxLines; ++k) {
            if (!page.empty()) page.push_back('\n');
            page += lines[k];
        }
        pages.push_back(std::move(page));
    }
    return pages;
}

}  // namespace hb::captions
