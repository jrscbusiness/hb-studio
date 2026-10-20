#include "ptz/preset_store.hpp"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

namespace hb::ptz {

namespace {

// ---------------- minimal JSON value + parser ----------------
struct J {
    enum class T { Null, Bool, Num, Str, Arr, Obj } t = T::Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<J> a;
    std::vector<std::pair<std::string, J>> o;

    const J* get(const char* key) const {
        if (t != T::Obj) return nullptr;
        for (auto& kv : o)
            if (kv.first == key) return &kv.second;
        return nullptr;
    }
};

class Parser {
public:
    explicit Parser(const std::string& s) : s_(s) {}
    bool parse(J& out) {
        if (!value(out, 0)) return false;
        ws();
        return p_ == s_.size();
    }

private:
    static constexpr int kMaxDepth = 32;
    const std::string& s_;
    std::size_t p_ = 0;

    void ws() {
        while (p_ < s_.size() && (s_[p_] == ' ' || s_[p_] == '\t' || s_[p_] == '\n' || s_[p_] == '\r')) ++p_;
    }
    bool lit(const char* w) {
        std::size_t n = 0;
        while (w[n]) ++n;
        if (s_.compare(p_, n, w) != 0) return false;
        p_ += n;
        return true;
    }
    bool value(J& v, int depth) {
        if (depth > kMaxDepth) return false;
        ws();
        if (p_ >= s_.size()) return false;
        const char c = s_[p_];
        if (c == '{') return object(v, depth);
        if (c == '[') return array(v, depth);
        if (c == '"') {
            v.t = J::T::Str;
            return string(v.s);
        }
        if (c == 't') { v.t = J::T::Bool; v.b = true; return lit("true"); }
        if (c == 'f') { v.t = J::T::Bool; v.b = false; return lit("false"); }
        if (c == 'n') { v.t = J::T::Null; return lit("null"); }
        return number(v);
    }
    bool number(J& v) {
        const std::size_t st = p_;
        while (p_ < s_.size() && std::string("+-0123456789.eE").find(s_[p_]) != std::string::npos) ++p_;
        if (st == p_) return false;
        double d = 0;
        auto r = std::from_chars(s_.data() + st, s_.data() + p_, d);
        if (r.ec != std::errc{} || r.ptr != s_.data() + p_ || !std::isfinite(d)) return false;
        v.t = J::T::Num;
        v.n = d;
        return true;
    }
    static void utf8(std::string& o, unsigned cp) {
        if (cp < 0x80) o += static_cast<char>(cp);
        else if (cp < 0x800) { o += static_cast<char>(0xC0 | (cp >> 6)); o += static_cast<char>(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) {
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
    bool hex4(unsigned& out) {
        if (p_ + 4 > s_.size()) return false;
        out = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = s_[p_++];
            out <<= 4;
            if (c >= '0' && c <= '9') out |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') out |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') out |= static_cast<unsigned>(c - 'A' + 10);
            else return false;
        }
        return true;
    }
    bool string(std::string& out) {
        ++p_;  // opening quote
        out.clear();
        while (p_ < s_.size()) {
            const char c = s_[p_++];
            if (c == '"') return true;
            if (static_cast<unsigned char>(c) < 0x20) return false;
            if (c != '\\') { out += c; continue; }
            if (p_ >= s_.size()) return false;
            const char e = s_[p_++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    unsigned cp = 0;
                    if (!hex4(cp)) return false;
                    if (cp >= 0xD800 && cp < 0xDC00) {  // surrogate pair
                        unsigned lo = 0;
                        if (p_ + 2 > s_.size() || s_[p_] != '\\' || s_[p_ + 1] != 'u') return false;
                        p_ += 2;
                        if (!hex4(lo) || lo < 0xDC00 || lo > 0xDFFF) return false;
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    utf8(out, cp);
                    break;
                }
                default: return false;
            }
        }
        return false;
    }
    bool array(J& v, int depth) {
        v.t = J::T::Arr;
        ++p_;
        ws();
        if (p_ < s_.size() && s_[p_] == ']') { ++p_; return true; }
        for (;;) {
            J e;
            if (!value(e, depth + 1)) return false;
            v.a.push_back(std::move(e));
            ws();
            if (p_ >= s_.size()) return false;
            if (s_[p_] == ',') { ++p_; continue; }
            if (s_[p_] == ']') { ++p_; return true; }
            return false;
        }
    }
    bool object(J& v, int depth) {
        v.t = J::T::Obj;
        ++p_;
        ws();
        if (p_ < s_.size() && s_[p_] == '}') { ++p_; return true; }
        for (;;) {
            ws();
            if (p_ >= s_.size() || s_[p_] != '"') return false;
            std::string k;
            if (!string(k)) return false;
            ws();
            if (p_ >= s_.size() || s_[p_] != ':') return false;
            ++p_;
            J e;
            if (!value(e, depth + 1)) return false;
            v.o.emplace_back(std::move(k), std::move(e));
            ws();
            if (p_ >= s_.size()) return false;
            if (s_[p_] == ',') { ++p_; continue; }
            if (s_[p_] == '}') { ++p_; return true; }
            return false;
        }
    }
};

// ---------------- writer ----------------
void esc(std::string& o, const std::string& s) {
    o += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) {
                    char b[8];
                    std::snprintf(b, sizeof b, "\\u%04x", c);
                    o += b;
                } else {
                    o += static_cast<char>(c);
                }
        }
    }
    o += '"';
}
std::string num(double d) {
    char b[40];
    auto r = std::to_chars(b, b + sizeof b, d);  // locale independent, shortest round-trip
    return std::string(b, r.ptr);
}

bool asInt(const J* j, int& out) {
    if (!j || j->t != J::T::Num || j->n != std::floor(j->n) || std::fabs(j->n) > 1e9) return false;
    out = static_cast<int>(j->n);
    return true;
}
bool asStr(const J* j, std::string& out) {
    if (!j || j->t != J::T::Str) return false;
    out = j->s;
    return true;
}

}  // namespace

// ---------------- store ----------------
CameraPtzConfig PresetStore::config(const std::string& id) const {
    auto it = cams_.find(id);
    if (it != cams_.end() && it->second.hasConfig) return it->second.config;
    CameraPtzConfig c;
    c.cameraId = id;
    c.normalize();
    return c;
}
void PresetStore::setConfig(CameraPtzConfig cfg) {
    cfg.normalize();
    auto& r = cams_[cfg.cameraId];
    r.config = cfg;
    r.hasConfig = true;
}
bool PresetStore::hasConfig(const std::string& id) const {
    auto it = cams_.find(id);
    return it != cams_.end() && it->second.hasConfig;
}
void PresetStore::set(const std::string& cam, PresetEntry e) { cams_[cam].presets[e.id] = std::move(e); }
bool PresetStore::remove(const std::string& cam, int id) {
    auto it = cams_.find(cam);
    return it != cams_.end() && it->second.presets.erase(id) > 0;
}
std::optional<PresetEntry> PresetStore::find(const std::string& cam, int id) const {
    auto it = cams_.find(cam);
    if (it == cams_.end()) return std::nullopt;
    auto p = it->second.presets.find(id);
    if (p == it->second.presets.end()) return std::nullopt;
    return p->second;
}
std::vector<PresetEntry> PresetStore::list(const std::string& cam) const {
    std::vector<PresetEntry> v;
    auto it = cams_.find(cam);
    if (it != cams_.end())
        for (auto& kv : it->second.presets) v.push_back(kv.second);
    return v;
}
std::vector<std::string> PresetStore::cameras() const {
    std::vector<std::string> v;
    for (auto& kv : cams_) v.push_back(kv.first);
    return v;
}

std::string PresetStore::toJson() const {
    std::string o = "{\n  \"schema\": " + std::to_string(kPresetSchemaVersion) + ",\n  \"cameras\": {";
    bool firstCam = true;
    for (auto& [id, rec] : cams_) {
        o += firstCam ? "\n    " : ",\n    ";
        firstCam = false;
        esc(o, id);
        o += ": {";
        if (rec.hasConfig) {
            const auto& c = rec.config;
            o += "\n      \"config\": {\"protocol\": ";
            esc(o, protocolName(c.protocol));
            o += ", \"host\": ";
            esc(o, c.host);
            o += ", \"port\": " + std::to_string(c.port) + ", \"address\": " + std::to_string(c.address) +
                 ", \"presetCount\": " + std::to_string(c.presetCount) +
                 ", \"presetBase\": " + std::to_string(c.presetBase) + ", \"storage\": ";
            esc(o, storageName(c.storage));
            o += "},";
        }
        o += "\n      \"presets\": [";
        bool firstP = true;
        for (auto& [pid, p] : rec.presets) {
            o += firstP ? "\n        " : ",\n        ";
            firstP = false;
            o += "{\"id\": " + std::to_string(pid) + ", \"name\": ";
            esc(o, p.name);
            if (p.position) {
                o += ", \"position\": {\"pan\": " + num(p.position->pan) + ", \"tilt\": " + num(p.position->tilt) +
                     ", \"zoom\": " + num(p.position->zoom) + ", \"units\": ";
                esc(o, p.position->units == PositionUnits::Normalized ? "normalized" : "raw");
                o += "}";
            }
            o += "}";
        }
        o += firstP ? "]" : "\n      ]";
        o += "\n    }";
    }
    o += firstCam ? "}\n}\n" : "\n  }\n}\n";
    return o;
}

LoadStatus PresetStore::fromJson(const std::string& text) {
    cams_.clear();
    skipped_ = 0;
    try {
        if (text.size() > (8u << 20)) return LoadStatus::Corrupt;
        J root;
        if (!Parser(text).parse(root) || root.t != J::T::Obj) return LoadStatus::Corrupt;
        int ver = 0;
        if (!asInt(root.get("schema"), ver) || ver < 1) return LoadStatus::Corrupt;
        if (ver > kPresetSchemaVersion) return LoadStatus::UnsupportedVersion;
        const J* cams = root.get("cameras");
        if (!cams || cams->t != J::T::Obj) return LoadStatus::Corrupt;

        for (auto& [id, cj] : cams->o) {
            if (cj.t != J::T::Obj) { ++skipped_; continue; }
            Record rec;
            if (const J* c = cj.get("config"); c && c->t == J::T::Obj) {
                CameraPtzConfig cfg;
                cfg.cameraId = id;
                std::string s;
                bool ok = true;
                if (asStr(c->get("protocol"), s)) ok &= parseProtocol(s, cfg.protocol);
                asStr(c->get("host"), cfg.host);
                asInt(c->get("port"), cfg.port);
                asInt(c->get("address"), cfg.address);
                asInt(c->get("presetCount"), cfg.presetCount);
                asInt(c->get("presetBase"), cfg.presetBase);
                if (asStr(c->get("storage"), s)) ok &= parseStorage(s, cfg.storage);
                if (ok) {
                    cfg.normalize();
                    rec.config = cfg;
                    rec.hasConfig = true;
                } else {
                    ++skipped_;
                }
            }
            if (const J* pl = cj.get("presets"); pl && pl->t == J::T::Arr) {
                for (auto& pj : pl->a) {
                    PresetEntry e;
                    if (pj.t != J::T::Obj || !asInt(pj.get("id"), e.id) || e.id < 0 || e.id > 255) {
                        ++skipped_;
                        continue;
                    }
                    asStr(pj.get("name"), e.name);
                    if (const J* pos = pj.get("position"); pos && pos->t == J::T::Obj) {
                        const J *a = pos->get("pan"), *b = pos->get("tilt"), *z = pos->get("zoom");
                        if (a && b && z && a->t == J::T::Num && b->t == J::T::Num && z->t == J::T::Num) {
                            PtzPosition pp{a->n, b->n, z->n, PositionUnits::ViscaRaw};
                            std::string u;
                            if (asStr(pos->get("units"), u) && u == "normalized") pp.units = PositionUnits::Normalized;
                            e.position = pp;
                        } else {
                            ++skipped_;
                        }
                    }
                    rec.presets[e.id] = std::move(e);
                }
            }
            cams_[id] = std::move(rec);
        }
        return LoadStatus::Ok;
    } catch (...) {
        cams_.clear();
        return LoadStatus::Corrupt;
    }
}

bool PresetStore::saveToFile(const std::string& path) const {
    try {
        const std::string tmp = path + ".tmp";
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            if (!f) return false;
            f << toJson();
            f.flush();
            if (!f) return false;
        }
        std::error_code ec;
        std::filesystem::rename(tmp, path, ec);  // atomic replace on POSIX; replaces on Windows
        if (ec) {
            std::filesystem::remove(tmp, ec);
            return false;
        }
        return true;
    } catch (...) {
        return false;
    }
}

LoadStatus PresetStore::loadFromFile(const std::string& path) {
    try {
        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) {
            cams_.clear();
            return LoadStatus::Missing;
        }
        std::ifstream f(path, std::ios::binary);
        if (!f) {
            cams_.clear();
            return LoadStatus::Corrupt;
        }
        std::stringstream ss;
        ss << f.rdbuf();
        f.close();
        const auto st = fromJson(ss.str());
        if (st == LoadStatus::Corrupt) {
            // Keep the broken file for inspection so the next save does not destroy it.
            std::filesystem::rename(path, path + ".corrupt", ec);
        }
        return st;
    } catch (...) {
        cams_.clear();
        return LoadStatus::Corrupt;
    }
}

}  // namespace hb::ptz
