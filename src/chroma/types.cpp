#include "chroma/types.hpp"

#include <algorithm>

namespace hb::chroma {

const char* modeName(Mode m) {
    switch (m) {
        case Mode::Green: return "green";
        case Mode::Blue: return "blue";
        case Mode::Ai: return "ai";
        default: return "off";
    }
}
const char* qualityName(Quality q) {
    switch (q) {
        case Quality::Low: return "low";
        case Quality::High: return "high";
        default: return "med";
    }
}
const char* replName(ReplKind r) {
    switch (r) {
        case ReplKind::Blur: return "blur";
        case ReplKind::Image: return "image";
        case ReplKind::Transparent: return "transparent";
        default: return "color";
    }
}

std::string pctEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) {
        if (c < 0x20 || c == 0x7F || c == ';' || c == '=' || c == '%' || c == '|') {
            o += '%';
            o += hex[c >> 4];
            o += hex[c & 15];
        } else {
            o += static_cast<char>(c);
        }
    }
    return o;
}

std::string pctDecode(const std::string& s) {
    auto hv = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
    };
    std::string o;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && hv(s[i + 1]) >= 0 && hv(s[i + 2]) >= 0) {
            const int d = hv(s[i + 1]) * 16 + hv(s[i + 2]);
            if (d >= 0x20 && d != 0x7F) o += static_cast<char>(d);   // never decode into control characters
            i += 2;
        } else {
            o += s[i];
        }
    }
    return o;
}

namespace {
bool parseInt(const std::string& v, int lo, int hi, int& out) {
    if (v.empty() || v.size() > 9) return false;
    std::size_t i = 0;
    bool neg = false;
    if (v[0] == '-') {
        neg = true;
        i = 1;
    }
    if (i >= v.size()) return false;
    long n = 0;
    for (; i < v.size(); ++i) {
        if (v[i] < '0' || v[i] > '9') return false;
        n = n * 10 + (v[i] - '0');
    }
    if (neg) n = -n;
    out = static_cast<int>(std::clamp<long>(n, lo, hi));
    return true;
}
bool parseColor(const std::string& v, std::uint32_t& out) {
    std::size_t i = (!v.empty() && v[0] == '#') ? 1 : 0;
    if (v.size() - i != 6) return false;
    std::uint32_t n = 0;
    for (; i < v.size(); ++i) {
        const char c = v[i];
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return false;
        n = n * 16 + static_cast<std::uint32_t>(d);
    }
    out = n;
    return true;
}
std::string sanitizePath(const std::string& p) {
    std::string o;
    for (unsigned char c : p) {
        if (c < 0x20 || c == 0x7F) continue;
        if (o.size() >= 400) break;
        o += static_cast<char>(c);
    }
    return o;
}
}  // namespace

bool setField(Settings& s, const std::string& key, const std::string& value) {
    int n = 0;
    if (key == "mode") {
        if (value == "off") s.mode = Mode::Off;
        else if (value == "green") s.mode = Mode::Green;
        else if (value == "blue") s.mode = Mode::Blue;
        else if (value == "ai") s.mode = Mode::Ai;
        else return false;
    } else if (key == "tol") {
        if (!parseInt(value, 0, 100, n)) return false;
        s.tolerance = n;
    } else if (key == "soft") {
        if (!parseInt(value, 0, 100, n)) return false;
        s.softness = n;
    } else if (key == "spill") {
        if (!parseInt(value, 0, 100, n)) return false;
        s.spill = n;
    } else if (key == "q") {
        if (value == "low") s.quality = Quality::Low;
        else if (value == "med") s.quality = Quality::Medium;
        else if (value == "high") s.quality = Quality::High;
        else return false;
    } else if (key == "repl") {
        if (value == "color") s.repl = ReplKind::Color;
        else if (value == "blur") s.repl = ReplKind::Blur;
        else if (value == "image") s.repl = ReplKind::Image;
        else if (value == "transparent") s.repl = ReplKind::Transparent;
        else return false;
    } else if (key == "color") {
        std::uint32_t c;
        if (!parseColor(value, c)) return false;
        s.color = c;
    } else if (key == "blur") {
        if (!parseInt(value, 0, 100, n)) return false;
        s.blur = n;
    } else if (key == "img") {
        s.image = sanitizePath(value);
    } else {
        return false;
    }
    return true;
}

Settings parseSettings(const std::string& text) {
    Settings s;
    std::size_t pos = 0;
    int fields = 0;
    while (pos <= text.size() && fields++ < 32) {
        std::size_t end = text.find(';', pos);
        if (end == std::string::npos) end = text.size();
        const std::string item = text.substr(pos, end - pos);
        const auto eq = item.find('=');
        if (eq != std::string::npos) setField(s, item.substr(0, eq), pctDecode(item.substr(eq + 1)));
        pos = end + 1;
    }
    return s;
}

std::string serializeSettings(const Settings& s) {
    static const char* hex = "0123456789abcdef";
    std::string c = "#";
    for (int sh = 20; sh >= 0; sh -= 4) c += hex[(s.color >> sh) & 15];
    std::string o = std::string("mode=") + modeName(s.mode) + ";tol=" + std::to_string(s.tolerance) +
                    ";soft=" + std::to_string(s.softness) + ";spill=" + std::to_string(s.spill) +
                    ";q=" + qualityName(s.quality) + ";repl=" + replName(s.repl) + ";color=" + c +
                    ";blur=" + std::to_string(s.blur);
    if (!s.image.empty()) o += ";img=" + pctEncode(s.image);
    return o;
}

QualityParams qualityParams(Quality q) {
    switch (q) {
        case Quality::Low: return {100, 0, 1, 140};   // ~10 inferences/s, light post-processing
        case Quality::High: return {33, 1, 2, 200};   // up to ~30 inferences/s
        default: return {66, 1, 1, 170};              // ~15 inferences/s
    }
}

}  // namespace hb::chroma
