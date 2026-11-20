#include "core/json.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>

namespace hb::core::json {

namespace {

class Parser {
public:
    Parser(std::string_view s, const Limits& l) : s_(s), lim_(l) {}
    bool run(Value& out) {
        if (s_.size() > lim_.maxBytes) return false;
        // optional UTF-8 BOM
        if (s_.size() >= 3 && static_cast<unsigned char>(s_[0]) == 0xEF && static_cast<unsigned char>(s_[1]) == 0xBB &&
            static_cast<unsigned char>(s_[2]) == 0xBF)
            i_ = 3;
        ws();
        if (!value(out, 0)) return false;
        ws();
        return i_ == s_.size();
    }

private:
    void ws() {
        while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' || s_[i_] == '\r')) ++i_;
    }
    bool lit(std::string_view w) {
        if (s_.compare(i_, w.size(), w) != 0) return false;
        i_ += w.size();
        return true;
    }
    static void utf8(std::string& o, unsigned cp) {
        if (cp < 0x80) {
            o += static_cast<char>(cp);
        } else if (cp < 0x800) {
            o += static_cast<char>(0xC0 | (cp >> 6));
            o += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            o += static_cast<char>(0xE0 | (cp >> 12));
            o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            o += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            o += static_cast<char>(0xF0 | (cp >> 18));
            o += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            o += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    bool hex4(unsigned& cp) {
        if (i_ + 4 > s_.size()) return false;
        cp = 0;
        for (int k = 0; k < 4; ++k) {
            const char h = s_[i_++];
            cp <<= 4;
            if (h >= '0' && h <= '9') cp |= static_cast<unsigned>(h - '0');
            else if (h >= 'a' && h <= 'f') cp |= static_cast<unsigned>(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F') cp |= static_cast<unsigned>(h - 'A' + 10);
            else return false;
        }
        return true;
    }
    bool str(std::string& out) {
        if (i_ >= s_.size() || s_[i_] != '"') return false;
        ++i_;
        while (i_ < s_.size()) {
            const char c = s_[i_++];
            if (c == '"') return true;
            if (static_cast<unsigned char>(c) < 0x20) return false;  // raw control characters are illegal
            if (c != '\\') {
                out += c;
                continue;
            }
            if (i_ >= s_.size()) return false;
            const char e = s_[i_++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'u': {
                    unsigned cp = 0;
                    if (!hex4(cp)) return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF) {  // surrogate pair
                        if (i_ + 2 > s_.size() || s_[i_] != '\\' || s_[i_ + 1] != 'u') return false;
                        i_ += 2;
                        unsigned lo = 0;
                        if (!hex4(lo) || lo < 0xDC00 || lo > 0xDFFF) return false;
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        return false;
                    }
                    utf8(out, cp);
                    break;
                }
                default: return false;
            }
        }
        return false;  // unterminated
    }
    bool value(Value& v, int depth) {
        if (depth > lim_.maxDepth || i_ >= s_.size()) return false;
        const char c = s_[i_];
        if (c == '{') {
            ++i_;
            v.t = Value::T::Obj;
            ws();
            if (i_ < s_.size() && s_[i_] == '}') { ++i_; return true; }
            for (;;) {
                ws();
                std::string k;
                if (!str(k)) return false;
                ws();
                if (i_ >= s_.size() || s_[i_++] != ':') return false;
                ws();
                Value c2;
                if (!value(c2, depth + 1)) return false;
                v.o[k] = std::move(c2);
                if (v.o.size() > lim_.maxArray) return false;
                ws();
                if (i_ >= s_.size()) return false;
                if (s_[i_] == ',') { ++i_; continue; }
                if (s_[i_] == '}') { ++i_; return true; }
                return false;
            }
        }
        if (c == '[') {
            ++i_;
            v.t = Value::T::Arr;
            ws();
            if (i_ < s_.size() && s_[i_] == ']') { ++i_; return true; }
            for (;;) {
                ws();
                Value c2;
                if (!value(c2, depth + 1)) return false;
                v.a.push_back(std::move(c2));
                if (v.a.size() > lim_.maxArray) return false;
                ws();
                if (i_ >= s_.size()) return false;
                if (s_[i_] == ',') { ++i_; continue; }
                if (s_[i_] == ']') { ++i_; return true; }
                return false;
            }
        }
        if (c == '"') { v.t = Value::T::Str; return str(v.s); }
        if (lit("true")) { v.t = Value::T::Bool; v.b = true; return true; }
        if (lit("false")) { v.t = Value::T::Bool; v.b = false; return true; }
        if (lit("null")) { v.t = Value::T::Null; return true; }
        // number: -?int frac? exp?
        const std::size_t st = i_;
        if (s_[i_] == '-') ++i_;
        if (i_ >= s_.size() || !std::isdigit(static_cast<unsigned char>(s_[i_]))) return false;
        if (s_[i_] == '0') {
            ++i_;   // JSON forbids leading zeros: "01" is invalid (the stray digit then fails the structure check)
            if (i_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[i_]))) return false;
        } else {
            while (i_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[i_]))) ++i_;
        }
        if (i_ < s_.size() && s_[i_] == '.') {
            ++i_;
            if (i_ >= s_.size() || !std::isdigit(static_cast<unsigned char>(s_[i_]))) return false;
            while (i_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[i_]))) ++i_;
        }
        if (i_ < s_.size() && (s_[i_] == 'e' || s_[i_] == 'E')) {
            ++i_;
            if (i_ < s_.size() && (s_[i_] == '+' || s_[i_] == '-')) ++i_;
            if (i_ >= s_.size() || !std::isdigit(static_cast<unsigned char>(s_[i_]))) return false;
            while (i_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[i_]))) ++i_;
        }
        v.n = std::strtod(std::string(s_.substr(st, i_ - st)).c_str(), nullptr);
        v.t = Value::T::Num;
        return true;
    }

    std::string_view s_;
    Limits lim_;
    std::size_t i_ = 0;
};

}  // namespace

bool parse(std::string_view text, Value& out, const Limits& limits) {
    Value v;
    try {
        if (!Parser(text, limits).run(v)) return false;
    } catch (...) {
        return false;
    }
    out = std::move(v);
    return true;
}

std::string escape(std::string_view s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20 || c == 0x7F) {
                    char b[8];
                    std::snprintf(b, sizeof b, "\\u%04x", c);
                    o += b;
                } else {
                    o += static_cast<char>(c);
                }
        }
    }
    return o;
}

namespace {
void dumpValue(const Value& v, bool pretty, int level, std::string& o) {
    auto nl = [&](int l) {
        if (!pretty) return;
        o += '\n';
        o.append(static_cast<std::size_t>(l) * 2, ' ');
    };
    switch (v.t) {
        case Value::T::Null: o += "null"; break;
        case Value::T::Bool: o += v.b ? "true" : "false"; break;
        case Value::T::Num: {
            char b[48];
            if (v.n == static_cast<double>(static_cast<long long>(v.n)) && v.n > -1e15 && v.n < 1e15)
                std::snprintf(b, sizeof b, "%lld", static_cast<long long>(v.n));
            else
                std::snprintf(b, sizeof b, "%.17g", v.n);
            o += b;
            break;
        }
        case Value::T::Str: o += '"'; o += escape(v.s); o += '"'; break;
        case Value::T::Arr:
            if (v.a.empty()) { o += "[]"; break; }
            o += '[';
            for (std::size_t i = 0; i < v.a.size(); ++i) {
                if (i) o += ',';
                nl(level + 1);
                dumpValue(v.a[i], pretty, level + 1, o);
            }
            nl(level);
            o += ']';
            break;
        case Value::T::Obj:
            if (v.o.empty()) { o += "{}"; break; }
            o += '{';
            bool first = true;
            for (const auto& [k, c] : v.o) {
                if (!first) o += ',';
                first = false;
                nl(level + 1);
                o += '"'; o += escape(k); o += pretty ? "\": " : "\":";
                dumpValue(c, pretty, level + 1, o);
            }
            nl(level);
            o += '}';
            break;
    }
}
}  // namespace

std::string dump(const Value& v, bool pretty) {
    std::string o;
    dumpValue(v, pretty, 0, o);
    return o;
}

}  // namespace hb::core::json
