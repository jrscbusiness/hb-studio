#include "app/stream_destinations.hpp"

#include "core/json.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace hb::app {

namespace {

using Json = hb::core::json::Value;
std::string esc(const std::string& s) { return hb::core::json::escape(s); }
const Json* jget(const Json& o, const char* k) { return o.get(k); }

std::string clean(const std::string& in, std::size_t maxLen, bool allowSlashesEtc) {
    std::string o;
    for (unsigned char c : in) {
        if (c < 0x20 || c == 0x7F) continue;            // no control chars (breaks JSON/log lines)
        if (!allowSlashesEtc && (c == '<' || c == '>')) continue;
        if (o.size() >= maxLen) break;
        o += static_cast<char>(c);
    }
    return o;
}

bool oneOf(const std::string& v, std::initializer_list<const char*> opts) {
    return std::find_if(opts.begin(), opts.end(), [&](const char* o) { return v == o; }) != opts.end();
}

StreamDestination sanitize(StreamDestination d) {
    d.name = clean(d.name, 64, true);
    if (!oneOf(d.platform, {"youtube", "facebook", "twitch", "kick", "vimeo", "linkedin", "x", "tiktok", "custom"}))
        d.platform = "custom";
    if (!oneOf(d.protocol, {"rtmp", "rtmps", "srt"})) d.protocol = "rtmp";
    d.serverUrl = clean(d.serverUrl, 256, true);
    d.streamKey = clean(d.streamKey, 256, true);
    if (!oneOf(d.encoder, {"auto", "x264", "nvenc", "qsv", "amf"})) d.encoder = "auto";
    d.videoKbps = std::clamp(d.videoKbps, 300, 60000);
    d.audioKbps = std::clamp(d.audioKbps, 32, 512);
    d.width = std::clamp(d.width, 160, 7680);
    d.height = std::clamp(d.height, 90, 4320);
    d.fpsNum = std::clamp(d.fpsNum, 1, 240);
    d.fpsDen = std::clamp(d.fpsDen, 1, 240);
    return d;
}

void readDest(const Json& o, StreamDestination& d) {
    if (auto* v = jget(o, "id"); v && v->t == Json::T::Str) d.id = v->s;
    if (auto* v = jget(o, "name"); v && v->t == Json::T::Str) d.name = v->s;
    if (auto* v = jget(o, "platform"); v && v->t == Json::T::Str) d.platform = v->s;
    if (auto* v = jget(o, "protocol"); v && v->t == Json::T::Str) d.protocol = v->s;
    if (auto* v = jget(o, "serverUrl"); v && v->t == Json::T::Str) d.serverUrl = v->s;
    if (auto* v = jget(o, "streamKey"); v && v->t == Json::T::Str) d.streamKey = v->s;
    if (auto* v = jget(o, "enabled"); v && v->t == Json::T::Bool) d.enabled = v->b;
    if (auto* v = jget(o, "encoder"); v && v->t == Json::T::Str) d.encoder = v->s;
    if (auto* v = jget(o, "videoKbps"); v && v->t == Json::T::Num) d.videoKbps = static_cast<int>(v->n);
    if (auto* v = jget(o, "audioKbps"); v && v->t == Json::T::Num) d.audioKbps = static_cast<int>(v->n);
    if (auto* v = jget(o, "adaptiveBitrate"); v && v->t == Json::T::Bool) d.adaptiveBitrate = v->b;
    if (auto* v = jget(o, "useOwnFormat"); v && v->t == Json::T::Bool) d.useOwnFormat = v->b;
    if (auto* v = jget(o, "width"); v && v->t == Json::T::Num) d.width = static_cast<int>(v->n);
    if (auto* v = jget(o, "height"); v && v->t == Json::T::Num) d.height = static_cast<int>(v->n);
    if (auto* v = jget(o, "fpsNum"); v && v->t == Json::T::Num) d.fpsNum = static_cast<int>(v->n);
    if (auto* v = jget(o, "fpsDen"); v && v->t == Json::T::Num) d.fpsDen = static_cast<int>(v->n);
}

std::string destJson(const StreamDestination& d) {
    std::ostringstream o;
    o << "\"id\":\"" << esc(d.id) << "\",\"name\":\"" << esc(d.name) << "\",\"platform\":\"" << esc(d.platform)
      << "\",\"protocol\":\"" << esc(d.protocol) << "\",\"serverUrl\":\"" << esc(d.serverUrl) << "\",\"streamKey\":\""
      << esc(d.streamKey) << "\",\"enabled\":" << (d.enabled ? "true" : "false") << ",\"encoder\":\"" << esc(d.encoder)
      << "\",\"videoKbps\":" << d.videoKbps << ",\"audioKbps\":" << d.audioKbps << ",\"adaptiveBitrate\":" << (d.adaptiveBitrate ? "true" : "false")
      << ",\"useOwnFormat\":"
      << (d.useOwnFormat ? "true" : "false") << ",\"width\":" << d.width << ",\"height\":" << d.height
      << ",\"fpsNum\":" << d.fpsNum << ",\"fpsDen\":" << d.fpsDen;
    return o.str();
}

}  // namespace

std::string maskSecret(const std::string& secret) {
    if (secret.empty()) return "";
    const std::size_t tailLen = std::min<std::size_t>(4, secret.size());
    return "••••" + secret.substr(secret.size() - tailLen);
}

std::vector<StreamDestination> StreamDestinations::list() const {
    std::lock_guard lock(mutex_);
    return d_;
}

StreamDestination StreamDestinations::get(const std::string& id) const {
    if (id.empty()) return {};
    std::lock_guard lock(mutex_);
    for (const auto& d : d_)
        if (d.id == id) return d;
    return {};
}

std::string StreamDestinations::upsert(StreamDestination d) {
    d = sanitize(std::move(d));
    if (d.name.empty() || d.serverUrl.empty()) return "";
    std::lock_guard lock(mutex_);
    if (!d.id.empty()) {
        for (auto& e : d_)
            if (e.id == d.id) { e = d; ++version_; return e.id; }
        return "";   // id given but not found: update rejected, never silently adds a duplicate
    }
    if (d_.size() >= static_cast<std::size_t>(kMax)) return "";
    d.id = "d" + std::to_string(version_);   // version_ only ever increases, so ids never collide
    d_.push_back(d);
    ++version_;
    return d.id;
}

bool StreamDestinations::remove(const std::string& id) {
    std::lock_guard lock(mutex_);
    const auto before = d_.size();
    d_.erase(std::remove_if(d_.begin(), d_.end(), [&](const auto& d) { return d.id == id; }), d_.end());
    if (d_.size() == before) return false;
    ++version_;
    return true;
}

bool StreamDestinations::setEnabled(const std::string& id, bool enabled) {
    std::lock_guard lock(mutex_);
    for (auto& d : d_)
        if (d.id == id) { d.enabled = enabled; ++version_; return true; }
    return false;
}

unsigned StreamDestinations::version() const {
    std::lock_guard lock(mutex_);
    return version_;
}

std::string StreamDestinations::toJson() const {
    std::lock_guard lock(mutex_);
    std::ostringstream o;
    o << "{\"schema\":1,\"nextSeq\":" << version_ << ",\"destinations\":[";
    for (std::size_t i = 0; i < d_.size(); ++i) {
        if (i) o << ",";
        o << "{" << destJson(d_[i]) << "}";
    }
    o << "]}";
    return o.str();
}

bool StreamDestinations::fromJson(const std::string& json) {
    Json root;
    if (!hb::core::json::parse(json, root) || root.t != Json::T::Obj) return false;
    auto* arr = jget(root, "destinations");
    if (!arr || arr->t != Json::T::Arr) return false;
    std::vector<StreamDestination> n;
    for (std::size_t i = 0; i < arr->a.size() && n.size() < static_cast<std::size_t>(kMax); ++i) {
        const Json& j = arr->a[i];
        if (j.t != Json::T::Obj) continue;
        StreamDestination d;
        readDest(j, d);
        d = sanitize(d);
        if (d.id.empty() || d.name.empty() || d.serverUrl.empty()) continue;   // hostile/truncated entry: skip it
        n.push_back(std::move(d));
    }
    unsigned nextSeq = 1;
    if (auto* v = jget(root, "nextSeq"); v && v->t == Json::T::Num) nextSeq = static_cast<unsigned>(v->n);
    std::lock_guard lock(mutex_);
    d_ = std::move(n);
    version_ = std::max<unsigned>(nextSeq, 1);
    return true;
}

bool StreamDestinations::load(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    if (ss.str().size() > 1024 * 1024) return false;
    return fromJson(ss.str());
}

bool StreamDestinations::save(const std::string& path) const {
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

bool StreamDestinations::parseDestination(const std::string& json, StreamDestination& out) {
    Json root;
    if (!hb::core::json::parse(json, root) || root.t != Json::T::Obj) return false;
    StreamDestination d;
    readDest(root, d);
    out = std::move(d);
    return true;
}

std::string StreamDestinations::uiJson() const {
    std::lock_guard lock(mutex_);
    std::ostringstream o;
    o << "{\"t\":\"xmit_dest\",\"list\":[";
    for (std::size_t i = 0; i < d_.size(); ++i) {
        const auto& d = d_[i];
        if (i) o << ",";
        o << "{\"id\":\"" << esc(d.id) << "\",\"name\":\"" << esc(d.name) << "\",\"platform\":\"" << esc(d.platform)
          << "\",\"protocol\":\"" << esc(d.protocol) << "\",\"serverUrl\":\"" << esc(d.serverUrl)
          << "\",\"keySet\":" << (d.streamKey.empty() ? "false" : "true") << ",\"keyHint\":\""
          << esc(maskSecret(d.streamKey)) << "\",\"enabled\":" << (d.enabled ? "true" : "false") << ",\"encoder\":\""
          << esc(d.encoder) << "\",\"videoKbps\":" << d.videoKbps << ",\"audioKbps\":" << d.audioKbps
          << ",\"adaptiveBitrate\":" << (d.adaptiveBitrate ? "true" : "false") << ",\"useOwnFormat\":" << (d.useOwnFormat ? "true" : "false") << ",\"width\":" << d.width
          << ",\"height\":" << d.height << ",\"fpsNum\":" << d.fpsNum << ",\"fpsDen\":" << d.fpsDen << "}";
    }
    o << "]}";
    return o.str();
}

}  // namespace hb::app
