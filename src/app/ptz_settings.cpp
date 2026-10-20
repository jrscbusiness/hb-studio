#include "app/ptz_settings.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <vector>

namespace hb::app {

namespace {

// ------------------------------------------------------------------ mini JSON
struct Json {
    enum class T { Null, Bool, Num, Str, Arr, Obj } t = T::Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Json> a;
    std::map<std::string, Json> o;
};

class Parser {
public:
    explicit Parser(const std::string& s) : s_(s) {}
    bool parse(Json& out) {
        ws();
        if (!value(out, 0)) return false;
        ws();
        return i_ == s_.size();
    }

private:
    void ws() { while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' || s_[i_] == '\r')) ++i_; }
    bool lit(const char* w) {
        const std::size_t n = std::char_traits<char>::length(w);
        if (s_.compare(i_, n, w) != 0) return false;
        i_ += n;
        return true;
    }
    static void utf8(std::string& o, unsigned cp) {
        if (cp < 0x80) o += static_cast<char>(cp);
        else if (cp < 0x800) { o += static_cast<char>(0xC0 | (cp >> 6)); o += static_cast<char>(0x80 | (cp & 0x3F)); }
        else { o += static_cast<char>(0xE0 | (cp >> 12)); o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); o += static_cast<char>(0x80 | (cp & 0x3F)); }
    }
    bool str(std::string& out) {
        if (i_ >= s_.size() || s_[i_] != '"') return false;
        ++i_;
        while (i_ < s_.size()) {
            char c = s_[i_++];
            if (c == '"') return true;
            if (c != '\\') { out += c; continue; }
            if (i_ >= s_.size()) return false;
            char e = s_[i_++];
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
                    if (i_ + 4 > s_.size()) return false;
                    unsigned cp = 0;
                    for (int k = 0; k < 4; ++k) {
                        const char h = s_[i_++];
                        cp <<= 4;
                        if (h >= '0' && h <= '9') cp |= static_cast<unsigned>(h - '0');
                        else if (h >= 'a' && h <= 'f') cp |= static_cast<unsigned>(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') cp |= static_cast<unsigned>(h - 'A' + 10);
                        else return false;
                    }
                    utf8(out, cp);
                    break;
                }
                default: return false;
            }
        }
        return false;
    }
    bool value(Json& v, int depth) {
        if (depth > 16 || i_ >= s_.size()) return false;  // bounded recursion: hostile input safe
        const char c = s_[i_];
        if (c == '{') {
            ++i_; v.t = Json::T::Obj; ws();
            if (i_ < s_.size() && s_[i_] == '}') { ++i_; return true; }
            for (;;) {
                ws();
                std::string k;
                if (!str(k)) return false;
                ws();
                if (i_ >= s_.size() || s_[i_++] != ':') return false;
                ws();
                Json c2;
                if (!value(c2, depth + 1)) return false;
                v.o[k] = std::move(c2);
                ws();
                if (i_ >= s_.size()) return false;
                if (s_[i_] == ',') { ++i_; continue; }
                if (s_[i_] == '}') { ++i_; return true; }
                return false;
            }
        }
        if (c == '[') {
            ++i_; v.t = Json::T::Arr; ws();
            if (i_ < s_.size() && s_[i_] == ']') { ++i_; return true; }
            for (;;) {
                ws();
                Json c2;
                if (!value(c2, depth + 1)) return false;
                v.a.push_back(std::move(c2));
                if (v.a.size() > 4096) return false;
                ws();
                if (i_ >= s_.size()) return false;
                if (s_[i_] == ',') { ++i_; continue; }
                if (s_[i_] == ']') { ++i_; return true; }
                return false;
            }
        }
        if (c == '"') { v.t = Json::T::Str; return str(v.s); }
        if (lit("true")) { v.t = Json::T::Bool; v.b = true; return true; }
        if (lit("false")) { v.t = Json::T::Bool; v.b = false; return true; }
        if (lit("null")) { v.t = Json::T::Null; return true; }
        // number
        const std::size_t st = i_;
        if (s_[i_] == '-') ++i_;
        while (i_ < s_.size() && (std::isdigit(static_cast<unsigned char>(s_[i_])) || s_[i_] == '.' || s_[i_] == 'e' || s_[i_] == 'E' || s_[i_] == '+' || s_[i_] == '-')) ++i_;
        if (i_ == st) return false;
        char* end = nullptr;
        v.n = std::strtod(s_.substr(st, i_ - st).c_str(), &end);
        v.t = Json::T::Num;
        return true;
    }
    const std::string& s_;
    std::size_t i_ = 0;
};

std::string esc(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
                else o += static_cast<char>(c);
        }
    }
    return o;
}

const Json* get(const Json& o, const char* k) {
    auto it = o.o.find(k);
    return it == o.o.end() ? nullptr : &it->second;
}

std::string cleanName(const std::string& in) {
    std::string o;
    std::size_t chars = 0;
    for (std::size_t i = 0; i < in.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(in[i]);
        if (c < 0x20 || c == 0x7F) continue;
        const bool lead = (c & 0xC0) != 0x80;       // not a UTF-8 continuation byte
        if (lead && chars >= 24) break;             // never cut a code point in half
        o += static_cast<char>(c);
        if (lead) ++chars;
    }
    const auto b = o.find_first_not_of(' ');
    if (b == std::string::npos) return {};
    const auto e = o.find_last_not_of(' ');
    return o.substr(b, e - b + 1);
}

void readConfig(const Json& o, PtzCameraConfig& c) {
    if (auto* v = get(o, "protocol"); v && v->t == Json::T::Str) c.protocol = v->s;
    if (auto* v = get(o, "host"); v && v->t == Json::T::Str) c.host = v->s;
    if (auto* v = get(o, "port"); v && v->t == Json::T::Num) c.port = static_cast<int>(v->n);
    if (auto* v = get(o, "address"); v && v->t == Json::T::Num) c.address = static_cast<int>(v->n);
    if (auto* v = get(o, "slots"); v && v->t == Json::T::Num) c.slotCount = static_cast<int>(v->n);
    if (auto* v = get(o, "base"); v && v->t == Json::T::Num) c.presetBase = static_cast<int>(v->n);
    if (auto* v = get(o, "storage"); v && v->t == Json::T::Str) c.storage = v->s;
    if (auto* v = get(o, "speed"); v && v->t == Json::T::Num) c.speed = static_cast<int>(v->n);
    if (auto* v = get(o, "invPan"); v && v->t == Json::T::Bool) c.invertPan = v->b;
    if (auto* v = get(o, "invTilt"); v && v->t == Json::T::Bool) c.invertTilt = v->b;
}

std::string configJson(const PtzCameraConfig& c) {
    std::ostringstream o;
    o << "\"protocol\":\"" << esc(c.protocol) << "\",\"host\":\"" << esc(c.host) << "\",\"port\":" << c.port
      << ",\"address\":" << c.address << ",\"slots\":" << c.slotCount << ",\"base\":" << c.presetBase
      << ",\"storage\":\"" << esc(c.storage) << "\",\"speed\":" << c.speed
      << ",\"invPan\":" << (c.invertPan ? "true" : "false") << ",\"invTilt\":" << (c.invertTilt ? "true" : "false");
    return o.str();
}

}  // namespace

int defaultPtzPort(const std::string& p) {
    if (p == "onvif") return 80;
    if (p == "visca-ip") return 52381;
    return 0;
}

PtzCameraConfig sanitize(PtzCameraConfig c) {
    static const char* kProtocols[] = {"onvif", "visca-ip", "visca-serial", "pelco-d", "pelco-p"};
    if (std::find_if(std::begin(kProtocols), std::end(kProtocols), [&](const char* p) { return c.protocol == p; }) ==
        std::end(kProtocols))
        c.protocol = "onvif";
    std::string h;
    for (unsigned char ch : c.host)
        if (ch > 0x20 && ch != 0x7F && ch != '"' && ch != '\\' && ch != '<' && ch != '>' && h.size() < 253)
            h += static_cast<char>(ch);
    c.host = h;
    c.port = std::clamp(c.port, 0, 65535);
    c.address = std::clamp(c.address, 1, 255);
    c.slotCount = std::clamp(c.slotCount, 10, PtzSettings::kMaxSlots);
    c.presetBase = c.presetBase == 0 ? 0 : 1;
    if (c.storage != "software") c.storage = "camera";
    c.speed = std::clamp(c.speed, 1, 100);
    return c;
}

PtzCameraConfig PtzSettings::config(int cam) const {
    std::lock_guard lock(mutex_);
    return cam >= 0 && cam < kCameras ? cfg_[static_cast<std::size_t>(cam)] : PtzCameraConfig{};
}

bool PtzSettings::setConfig(int cam, const PtzCameraConfig& c) {
    if (cam < 0 || cam >= kCameras) return false;
    std::lock_guard lock(mutex_);
    cfg_[static_cast<std::size_t>(cam)] = sanitize(c);
    ++version_;
    return true;
}

void PtzSettings::applyToAll(const PtzCameraConfig& c) {
    std::lock_guard lock(mutex_);
    for (auto& x : cfg_) x = sanitize(c);
    ++version_;
}

std::string PtzSettings::presetName(int cam, int slot) const {
    if (cam < 0 || cam >= kCameras || slot < 0 || slot >= kMaxSlots) return {};
    std::lock_guard lock(mutex_);
    return names_[static_cast<std::size_t>(cam)][static_cast<std::size_t>(slot)];
}

bool PtzSettings::setPresetName(int cam, int slot, std::string name) {
    if (cam < 0 || cam >= kCameras || slot < 0 || slot >= kMaxSlots) return false;
    std::lock_guard lock(mutex_);
    names_[static_cast<std::size_t>(cam)][static_cast<std::size_t>(slot)] = cleanName(name);
    ++version_;
    return true;
}

unsigned PtzSettings::version() const {
    std::lock_guard lock(mutex_);
    return version_;
}

std::string PtzSettings::toJson() const {
    std::lock_guard lock(mutex_);
    std::ostringstream o;
    o << "{\"schema\":1,\"cameras\":[";
    for (int c = 0; c < kCameras; ++c) {
        if (c) o << ",";
        o << "{" << configJson(cfg_[static_cast<std::size_t>(c)]) << ",\"names\":[";
        for (int s = 0; s < kMaxSlots; ++s) {
            if (s) o << ",";
            o << "\"" << esc(names_[static_cast<std::size_t>(c)][static_cast<std::size_t>(s)]) << "\"";
        }
        o << "]}";
    }
    o << "]}";
    return o.str();
}

bool PtzSettings::fromJson(const std::string& json) {
    Json root;
    if (!Parser(json).parse(root) || root.t != Json::T::Obj) return false;
    auto* cams = get(root, "cameras");
    if (!cams || cams->t != Json::T::Arr) return false;
    std::array<PtzCameraConfig, kCameras> cfg{};
    std::array<std::array<std::string, kMaxSlots>, kCameras> names{};
    for (std::size_t c = 0; c < cams->a.size() && c < static_cast<std::size_t>(kCameras); ++c) {
        const Json& j = cams->a[c];
        if (j.t != Json::T::Obj) continue;
        readConfig(j, cfg[c]);
        cfg[c] = sanitize(cfg[c]);
        if (auto* n = get(j, "names"); n && n->t == Json::T::Arr)
            for (std::size_t s = 0; s < n->a.size() && s < static_cast<std::size_t>(kMaxSlots); ++s)
                if (n->a[s].t == Json::T::Str) names[c][s] = cleanName(n->a[s].s);
    }
    std::lock_guard lock(mutex_);
    cfg_ = cfg;
    names_ = names;
    ++version_;
    return true;
}

bool PtzSettings::parseConfig(const std::string& json, PtzCameraConfig& out) {
    Json root;
    if (!Parser(json).parse(root) || root.t != Json::T::Obj) return false;
    PtzCameraConfig c;
    readConfig(root, c);
    out = sanitize(c);
    return true;
}

bool PtzSettings::load(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    if (ss.str().size() > 4 * 1024 * 1024) return false;
    return fromJson(ss.str());
}

bool PtzSettings::save(const std::string& path) const {
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << toJson();
        if (!f.good()) return false;
    }
    std::remove(path.c_str());
    return std::rename(tmp.c_str(), path.c_str()) == 0;
}

std::string PtzSettings::uiJson() const {
    std::lock_guard lock(mutex_);
    std::ostringstream o;
    o << "{\"t\":\"ptz\",\"cams\":[";
    for (int c = 0; c < kCameras; ++c) {
        if (c) o << ",";
        const auto& cf = cfg_[static_cast<std::size_t>(c)];
        o << "{" << configJson(cf) << ",\"defPort\":" << defaultPtzPort(cf.protocol) << ",\"connected\":false,\"names\":[";
        for (int s = 0; s < cf.slotCount; ++s) {
            if (s) o << ",";
            o << "\"" << esc(names_[static_cast<std::size_t>(c)][static_cast<std::size_t>(s)]) << "\"";
        }
        o << "]}";
    }
    o << "]}";
    return o.str();
}

}  // namespace hb::app
