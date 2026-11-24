#include "overlay/text.hpp"

#include <algorithm>
#include <cstring>

namespace hb::overlay {

std::string TextStyle::key() const {
    std::string k = font;
    for (long long v : {static_cast<long long>(px), static_cast<long long>(bold), static_cast<long long>(color),
                        static_cast<long long>(outlinePx), static_cast<long long>(outlineColor), static_cast<long long>(shadowPx),
                        static_cast<long long>(shadowColor), static_cast<long long>(shadowAlpha), static_cast<long long>(align),
                        static_cast<long long>(maxLines), static_cast<long long>(keepTail), static_cast<long long>(lineSpacingPct)}) {
        k += '|';
        k += std::to_string(v);
    }
    return k;
}

// ---------------- UTF-8 ----------------
std::size_t utf8SeqLen(std::string_view s, std::size_t i) {
    if (i >= s.size()) return 0;
    const unsigned char c = static_cast<unsigned char>(s[i]);
    std::size_t n = 1;
    if (c >= 0xF0 && c <= 0xF4) n = 4;
    else if (c >= 0xE0) n = c < 0xF0 ? 3 : 1;
    else if (c >= 0xC2) n = c < 0xE0 ? 2 : 1;
    if (n == 1 || i + n > s.size()) return 1;
    for (std::size_t k = 1; k < n; ++k)
        if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return 1;
    return n;
}

std::uint32_t utf8Decode(std::string_view s, std::size_t i) {
    const std::size_t n = utf8SeqLen(s, i);
    if (n == 0) return 0xFFFD;
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (n == 1) return c < 0x80 ? c : 0xFFFD;
    std::uint32_t cp = n == 2 ? (c & 0x1F) : (n == 3 ? (c & 0x0F) : (c & 0x07));
    for (std::size_t k = 1; k < n; ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
    if ((n == 3 && cp < 0x800) || (n == 4 && (cp < 0x10000 || cp > 0x10FFFF)) || (cp >= 0xD800 && cp <= 0xDFFF)) return 0xFFFD;
    return cp;
}

std::string sanitizeUtf8(std::string_view s, std::size_t maxBytes) {
    std::string out;
    for (std::size_t i = 0; i < s.size() && out.size() < maxBytes;) {
        const std::size_t n = utf8SeqLen(s, i);
        const std::uint32_t cp = utf8Decode(s, i);
        if (cp == 0xFFFD) { out += "\xEF\xBF\xBD"; i += n; continue; }
        if (cp < 0x20 && cp != '\n') { i += n; if (cp == '\t') out += ' '; continue; }
        if (cp == 0x7F) { i += n; continue; }
        if (out.size() + n > maxBytes) break;
        out.append(s.substr(i, n));
        i += n;
    }
    return out;
}

// ---------------- word wrap ----------------
std::vector<std::string> MaskTextRasterizer::wrap(const std::string& text, const TextStyle& st, int maxWidth) {
    std::vector<std::string> lines;
    std::size_t p = 0;
    while (p <= text.size()) {
        std::size_t e = text.find('\n', p);
        if (e == std::string::npos) e = text.size();
        const std::string para = text.substr(p, e - p);
        p = e + 1;
        if (maxWidth <= 0 || glyphs_->advance(para, st) <= maxWidth) { lines.push_back(para); continue; }
        std::string cur;
        std::size_t i = 0;
        while (i < para.size()) {
            // next token: a run of spaces or a word
            std::size_t j = i;
            const bool space = para[i] == ' ';
            while (j < para.size() && (para[j] == ' ') == space) ++j;
            std::string tok = para.substr(i, j - i);
            i = j;
            if (space) { if (!cur.empty()) cur += tok; continue; }
            std::string trial = cur + tok;
            if (glyphs_->advance(trial, st) <= maxWidth) { cur = trial; continue; }
            if (!cur.empty()) {  // wrap before this word
                while (!cur.empty() && cur.back() == ' ') cur.pop_back();
                lines.push_back(cur);
                cur.clear();
            }
            // the word itself may be wider than a line: split by code point
            std::size_t k = 0;
            std::string piece;
            while (k < tok.size()) {
                const std::size_t n = utf8SeqLen(tok, k);
                std::string next = piece + tok.substr(k, n);
                if (!piece.empty() && glyphs_->advance(next, st) > maxWidth) { lines.push_back(piece); piece.clear(); continue; }
                piece = next;
                k += n;
            }
            cur = piece;
        }
        while (!cur.empty() && cur.back() == ' ') cur.pop_back();
        lines.push_back(cur);
    }
    if (st.maxLines > 0 && static_cast<int>(lines.size()) > st.maxLines) {
        if (st.keepTail) lines.erase(lines.begin(), lines.end() - st.maxLines);
        else lines.resize(static_cast<std::size_t>(st.maxLines));
    }
    return lines;
}

namespace {
int margin(const TextStyle& s) { return std::max(0, s.outlinePx) + std::max(0, s.shadowPx); }
}  // namespace

bool MaskTextRasterizer::measure(const std::string& utf8, const TextStyle& st, int maxWidth, TextMetrics& out) {
    out = {};
    if (!glyphs_ || utf8.empty()) return false;
    const int m = margin(st);
    const auto lines = wrap(utf8, st, maxWidth > 0 ? maxWidth - 2 * m : 0);
    const int lh = glyphs_->lineHeight(st);
    int w = 0;
    for (const auto& l : lines) w = std::max(w, glyphs_->advance(l, st));
    const int pitch = std::max(1, lh * st.lineSpacingPct / 100);
    out.lines = static_cast<int>(lines.size());
    out.width = w + 2 * m;
    out.height = lh + pitch * std::max(0, out.lines - 1) + 2 * m;
    return out.lines > 0 && w > 0;
}

bool MaskTextRasterizer::rasterize(const std::string& utf8, const TextStyle& st, int maxWidth, Bitmap& out) {
    out = {};
    TextMetrics tm;
    if (!measure(utf8, st, maxWidth, tm) || tm.width > 8192 || tm.height > 4096) return false;
    const int m = margin(st);
    const auto lines = wrap(utf8, st, maxWidth > 0 ? maxWidth - 2 * m : 0);
    const int lh = glyphs_->lineHeight(st);
    const int pitch = std::max(1, lh * st.lineSpacingPct / 100);
    const int W = tm.width, H = tm.height;
    std::vector<std::uint8_t> base(static_cast<std::size_t>(W) * H, 0);
    for (std::size_t li = 0; li < lines.size(); ++li) {
        if (lines[li].empty()) continue;
        std::vector<std::uint8_t> mask;
        int mw = 0, mh = 0;
        if (!glyphs_->drawMask(lines[li], st, mask, mw, mh) || mw <= 0 || mh <= 0 ||
            mask.size() < static_cast<std::size_t>(mw) * mh) continue;
        const int inner = W - 2 * m;
        int ox = m;
        if (st.align == Align::Center) ox = m + (inner - mw) / 2;
        else if (st.align == Align::Right) ox = m + inner - mw;
        const int oy = m + static_cast<int>(li) * pitch;
        for (int y = 0; y < mh; ++y) {
            const int yy = oy + y;
            if (yy < 0 || yy >= H) continue;
            for (int x = 0; x < mw; ++x) {
                const int xx = ox + x;
                if (xx < 0 || xx >= W) continue;
                auto& d = base[static_cast<std::size_t>(yy) * W + xx];
                d = std::max(d, mask[static_cast<std::size_t>(y) * mw + x]);
            }
        }
    }
    // Outline: separable square max-filter (dilation) of the text coverage.
    std::vector<std::uint8_t> outline;
    const int r = std::max(0, st.outlinePx);
    if (r > 0) {
        std::vector<std::uint8_t> tmp(base.size(), 0);
        outline.assign(base.size(), 0);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                std::uint8_t v = 0;
                for (int k = std::max(0, x - r); k <= std::min(W - 1, x + r); ++k) v = std::max(v, base[static_cast<std::size_t>(y) * W + k]);
                tmp[static_cast<std::size_t>(y) * W + x] = v;
            }
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                std::uint8_t v = 0;
                for (int k = std::max(0, y - r); k <= std::min(H - 1, y + r); ++k) v = std::max(v, tmp[static_cast<std::size_t>(k) * W + x]);
                outline[static_cast<std::size_t>(y) * W + x] = v;
            }
    }
    const std::vector<std::uint8_t>& shape = r > 0 ? outline : base;  // what casts the shadow
    out.resize(W, H);
    const int s = std::max(0, st.shadowPx);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            std::uint8_t* d = out.bgra.data() + (static_cast<std::size_t>(y) * W + x) * 4;
            if (s > 0 && x >= s && y >= s) {
                const int a = shape[static_cast<std::size_t>(y - s) * W + (x - s)] * std::clamp(st.shadowAlpha, 0, 255) / 255;
                blendPixel(d, colB(st.shadowColor), colG(st.shadowColor), colR(st.shadowColor), a);
            }
            if (r > 0) blendPixel(d, colB(st.outlineColor), colG(st.outlineColor), colR(st.outlineColor), outline[static_cast<std::size_t>(y) * W + x]);
            blendPixel(d, colB(st.color), colG(st.color), colR(st.color), base[static_cast<std::size_t>(y) * W + x]);
        }
    return true;
}

// ---------------- built-in 5x7 font ----------------
namespace {

const char* glyphRows(char32_t c) {
    switch (c) {
        case '0': return "01110/10001/10011/10101/11001/10001/01110";
        case '1': return "00100/01100/00100/00100/00100/00100/01110";
        case '2': return "01110/10001/00001/00010/00100/01000/11111";
        case '3': return "11110/00001/00001/01110/00001/00001/11110";
        case '4': return "00010/00110/01010/10010/11111/00010/00010";
        case '5': return "11111/10000/11110/00001/00001/10001/01110";
        case '6': return "00110/01000/10000/11110/10001/10001/01110";
        case '7': return "11111/00001/00010/00100/01000/01000/01000";
        case '8': return "01110/10001/10001/01110/10001/10001/01110";
        case '9': return "01110/10001/10001/01111/00001/00010/01100";
        case 'A': return "01110/10001/10001/11111/10001/10001/10001";
        case 'B': return "11110/10001/10001/11110/10001/10001/11110";
        case 'C': return "01110/10001/10000/10000/10000/10001/01110";
        case 'D': return "11100/10010/10001/10001/10001/10010/11100";
        case 'E': return "11111/10000/10000/11110/10000/10000/11111";
        case 'F': return "11111/10000/10000/11110/10000/10000/10000";
        case 'G': return "01110/10001/10000/10111/10001/10001/01111";
        case 'H': return "10001/10001/10001/11111/10001/10001/10001";
        case 'I': return "01110/00100/00100/00100/00100/00100/01110";
        case 'J': return "00111/00010/00010/00010/00010/10010/01100";
        case 'K': return "10001/10010/10100/11000/10100/10010/10001";
        case 'L': return "10000/10000/10000/10000/10000/10000/11111";
        case 'M': return "10001/11011/10101/10101/10001/10001/10001";
        case 'N': return "10001/10001/11001/10101/10011/10001/10001";
        case 'O': return "01110/10001/10001/10001/10001/10001/01110";
        case 'P': return "11110/10001/10001/11110/10000/10000/10000";
        case 'Q': return "01110/10001/10001/10001/10101/10010/01101";
        case 'R': return "11110/10001/10001/11110/10100/10010/10001";
        case 'S': return "01111/10000/10000/01110/00001/00001/11110";
        case 'T': return "11111/00100/00100/00100/00100/00100/00100";
        case 'U': return "10001/10001/10001/10001/10001/10001/01110";
        case 'V': return "10001/10001/10001/10001/10001/01010/00100";
        case 'W': return "10001/10001/10001/10101/10101/10101/01010";
        case 'X': return "10001/10001/01010/00100/01010/10001/10001";
        case 'Y': return "10001/10001/01010/00100/00100/00100/00100";
        case 'Z': return "11111/00001/00010/00100/01000/10000/11111";
        case ':': return "00000/00100/00100/00000/00100/00100/00000";
        case '-': return "00000/00000/00000/11111/00000/00000/00000";
        case '.': return "00000/00000/00000/00000/00000/00110/00110";
        case ',': return "00000/00000/00000/00000/00110/00100/01000";
        case '+': return "00000/00100/00100/11111/00100/00100/00000";
        case '/': return "00001/00010/00010/00100/01000/01000/10000";
        case '!': return "00100/00100/00100/00100/00100/00000/00100";
        case '?': return "01110/10001/00001/00110/00100/00000/00100";
        case ' ': return "00000/00000/00000/00000/00000/00000/00000";
        default: return nullptr;
    }
}

class BuiltinGlyphs : public IGlyphSource {
public:
    static int scaleFor(const TextStyle& s) { return std::max(1, (s.px + 4) / 8); }
    int lineHeight(const TextStyle& s) override { return 8 * scaleFor(s); }
    int advance(const std::string& line, const TextStyle& s) override {
        int n = 0;
        for (std::size_t i = 0; i < line.size(); i += utf8SeqLen(line, i)) ++n;
        return n * 6 * scaleFor(s);
    }
    bool drawMask(const std::string& line, const TextStyle& s, std::vector<std::uint8_t>& mask, int& w, int& h) override {
        const int sc = scaleFor(s);
        w = advance(line, s);
        h = lineHeight(s);
        if (w <= 0) return false;
        mask.assign(static_cast<std::size_t>(w) * h, 0);
        int col = 0;
        for (std::size_t i = 0; i < line.size(); i += utf8SeqLen(line, i), ++col) {
            std::uint32_t cp = utf8Decode(line, i);
            if (cp >= 'a' && cp <= 'z') cp -= 32;
            const char* rows = glyphRows(cp);
            char boxed[48];
            if (!rows) {  // placeholder: box with a pattern derived from the code point (accents, emoji, ...)
                std::string g;
                for (int y = 0; y < 7; ++y) {
                    for (int x = 0; x < 5; ++x) g += (y == 0 || y == 6 || x == 0 || x == 4 || ((cp >> ((x + y) % 16)) & 1)) ? '1' : '0';
                    if (y < 6) g += '/';
                }
                std::strncpy(boxed, g.c_str(), sizeof(boxed) - 1);
                boxed[sizeof(boxed) - 1] = 0;
                rows = boxed;
            }
            for (int y = 0; y < 7; ++y)
                for (int x = 0; x < 5; ++x)
                    if (rows[y * 6 + x] == '1')
                        for (int dy = 0; dy < sc; ++dy)
                            for (int dx = 0; dx < sc; ++dx) {
                                const int px = col * 6 * sc + x * sc + dx, py = y * sc + sc / 2 + dy;
                                if (px < w && py < h) mask[static_cast<std::size_t>(py) * w + px] = 255;
                            }
        }
        return true;
    }
};

}  // namespace

std::shared_ptr<IGlyphSource> makeBuiltinGlyphSource() { return std::make_shared<BuiltinGlyphs>(); }
std::shared_ptr<ITextRasterizer> makeBuiltinTextRasterizer() { return std::make_shared<MaskTextRasterizer>(makeBuiltinGlyphSource()); }

}  // namespace hb::overlay
