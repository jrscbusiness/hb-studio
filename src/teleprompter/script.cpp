#include "teleprompter/script.hpp"

#include <cstdint>

namespace hb::teleprompter {

namespace {
bool isSpace(unsigned char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f'; }

std::string_view trim(std::string_view s) {
    while (!s.empty() && isSpace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && isSpace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

void appendUtf8(std::string& out, std::uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

// Windows-1252 bytes 0x80..0x9F -> Unicode (the five undefined slots map to the same C1 code point).
constexpr std::uint16_t kCp1252[32] = {0x20AC, 0x81,   0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
                                       0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x8D,   0x017D, 0x8F,
                                       0x90,   0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
                                       0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x9D,   0x017E, 0x0178};
}  // namespace

int countWords(std::string_view text) {
    int n = 0;
    bool in = false;
    for (unsigned char c : text) {
        if (isSpace(c)) in = false;
        else if (!in) { in = true; ++n; }
    }
    return n;
}

ParsedScript parseScript(std::string_view text) {
    ParsedScript out;
    std::string para;
    int paraLine = 1;
    auto flush = [&] {
        if (para.empty()) return;
        Block b;
        b.kind = Block::Kind::Paragraph;
        b.words = countWords(para);
        b.text = std::move(para);
        b.line = paraLine;
        para.clear();
        out.totalWords += b.words;
        ++out.paragraphs;
        out.blocks.push_back(std::move(b));
    };
    int lineNo = 0;
    std::size_t pos = 0;
    while (pos <= text.size()) {
        std::size_t e = text.find('\n', pos);
        if (e == std::string_view::npos) e = text.size();
        ++lineNo;
        const std::string_view line = trim(text.substr(pos, e - pos));
        if (line.empty()) {
            flush();
        } else if (line.front() == '#') {
            flush();
            std::string_view c = line;
            while (!c.empty() && c.front() == '#') c.remove_prefix(1);
            Block b;
            b.kind = Block::Kind::Cue;
            b.text = std::string(trim(c));
            b.line = lineNo;
            out.blocks.push_back(std::move(b));
        } else {
            if (para.empty()) paraLine = lineNo;
            else para += '\n';
            para.append(line);
        }
        if (e == text.size()) break;
        pos = e + 1;
    }
    flush();
    return out;
}

std::vector<double> paragraphStarts(const ParsedScript& s) {
    std::vector<double> r;
    int before = 0;
    for (const auto& b : s.blocks) {
        if (b.kind != Block::Kind::Paragraph) continue;
        r.push_back(s.totalWords > 0 ? static_cast<double>(before) / s.totalWords : 0.0);
        before += b.words;
    }
    return r;
}

bool isValidUtf8(std::string_view s) {
    std::size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) { ++i; continue; }
        std::size_t n = 0;
        std::uint32_t cp = 0;
        if (c >= 0xC2 && c <= 0xDF) { n = 1; cp = c & 0x1Fu; }
        else if (c >= 0xE0 && c <= 0xEF) { n = 2; cp = c & 0x0Fu; }
        else if (c >= 0xF0 && c <= 0xF4) { n = 3; cp = c & 0x07u; }
        else return false;
        if (i + n >= s.size()) return false;   // truncated sequence
        for (std::size_t k = 1; k <= n; ++k) {
            const unsigned char cc = static_cast<unsigned char>(s[i + k]);
            if ((cc & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (cc & 0x3Fu);
        }
        if ((n == 2 && cp < 0x800) || (n == 3 && cp < 0x10000) || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
            return false;
        i += n + 1;
    }
    return true;
}

std::string normalizeText(std::string_view raw) {
    if (raw.size() >= 3 && static_cast<unsigned char>(raw[0]) == 0xEF && static_cast<unsigned char>(raw[1]) == 0xBB &&
        static_cast<unsigned char>(raw[2]) == 0xBF)
        raw.remove_prefix(3);
    // Bound the work on hostile input: at most 2x the cap is examined (Latin-1 may grow when re-encoded).
    if (raw.size() > kMaxScriptBytes) raw = raw.substr(0, kMaxScriptBytes);
    std::string bytes;
    bytes.reserve(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        const char c = raw[i];
        if (c == '\0') continue;
        if (c == '\r') {
            bytes += '\n';
            if (i + 1 < raw.size() && raw[i + 1] == '\n') ++i;
        } else {
            bytes += c;
        }
    }
    if (!isValidUtf8(bytes)) {
        std::string out;
        out.reserve(bytes.size() * 2);
        for (unsigned char c : bytes) {
            if (c >= 0x80 && c <= 0x9F) appendUtf8(out, kCp1252[c - 0x80]);
            else appendUtf8(out, c);   // ASCII, and Latin-1 0xA0..0xFF map 1:1 to U+00A0..U+00FF
        }
        bytes = std::move(out);
    }
    if (bytes.size() > kMaxScriptBytes) {
        std::size_t cut = kMaxScriptBytes;
        while (cut > 0 && (static_cast<unsigned char>(bytes[cut]) & 0xC0) == 0x80) --cut;
        bytes.resize(cut);
    }
    return bytes;
}

}  // namespace hb::teleprompter
