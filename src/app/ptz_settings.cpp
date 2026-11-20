#include "app/ptz_settings.hpp"

#include "core/json.hpp"

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

using Json = hb::core::json::Value;
std::string esc(const std::string& s) { return hb::core::json::escape(s); }
const Json* get(const Json& o, const char* k) { return o.get(k); }

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
    if (!hb::core::json::parse(json, root) || root.t != Json::T::Obj) return false;
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
    if (!hb::core::json::parse(json, root) || root.t != Json::T::Obj) return false;
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
