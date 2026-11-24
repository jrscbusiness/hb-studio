#include "cloud/cloud_settings.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "core/json.hpp"

namespace hb::cloud {

using Json = hb::core::json::Value;

bool MemorySecretStore::put(const std::string& n, const std::string& v) {
    std::lock_guard lock(mu_);
    m_[n] = v;
    return true;
}
std::optional<std::string> MemorySecretStore::get(const std::string& n) {
    std::lock_guard lock(mu_);
    auto it = m_.find(n);
    if (it == m_.end()) return std::nullopt;
    return it->second;
}
bool MemorySecretStore::erase(const std::string& n) {
    std::lock_guard lock(mu_);
    return m_.erase(n) > 0;
}

std::string maskHint(const std::string& secret) {
    if (secret.empty()) return "";
    const std::size_t tail = std::min<std::size_t>(4, secret.size());
    return "••••" + secret.substr(secret.size() - tail);
}

namespace {
std::string clean(const std::string& in, std::size_t maxLen) {
    std::string o;
    for (unsigned char c : in) {
        if (c < 0x20 || c == 0x7F) continue;
        if (o.size() >= maxLen) break;
        o += static_cast<char>(c);
    }
    while (!o.empty() && o.back() == ' ') o.pop_back();
    while (!o.empty() && o.front() == ' ') o.erase(0, 1);
    return o;
}
}  // namespace

CloudSettings sanitize(CloudSettings s) {
    if (s.provider != "s3" && s.provider != "gdrive") s.provider = "s3";
    s.s3Endpoint = clean(s.s3Endpoint, 200);
    s.s3Region = clean(s.s3Region, 40);
    if (s.s3Region.empty()) s.s3Region = "us-east-1";
    s.s3Bucket = clean(s.s3Bucket, 63);
    s.s3Prefix = clean(s.s3Prefix, 120);
    if (s.s3Addressing != "auto" && s.s3Addressing != "path" && s.s3Addressing != "virtual") s.s3Addressing = "auto";
    s.partSizeMiB = std::clamp(s.partSizeMiB, 5, 512);
    s.gClientId = clean(s.gClientId, 200);
    s.gFolderName = clean(s.gFolderName, 64);
    if (s.gFolderName.empty()) s.gFolderName = "HB Studio";
    s.gFolderId = clean(s.gFolderId, 100);
    s.gAccount = clean(s.gAccount, 100);
    if (!(s.bandwidthMbps >= 0)) s.bandwidthMbps = 0;
    s.bandwidthMbps = std::min(s.bandwidthMbps, 10000.0);
    s.stableSeconds = std::clamp(s.stableSeconds, 10, 600);
    return s;
}

std::string settingsToJson(const CloudSettings& s) {
    Json o;
    o.t = Json::T::Obj;
    auto str = [&](const char* k, const std::string& v) { Json x; x.t = Json::T::Str; x.s = v; o.o[k] = x; };
    auto num = [&](const char* k, double v) { Json x; x.t = Json::T::Num; x.n = v; o.o[k] = x; };
    auto bol = [&](const char* k, bool v) { Json x; x.t = Json::T::Bool; x.b = v; o.o[k] = x; };
    num("schema", 1);
    bol("enabled", s.enabled);
    str("provider", s.provider);
    str("s3Endpoint", s.s3Endpoint);
    str("s3Region", s.s3Region);
    str("s3Bucket", s.s3Bucket);
    str("s3Prefix", s.s3Prefix);
    str("s3Addressing", s.s3Addressing);
    num("partSizeMiB", s.partSizeMiB);
    str("gClientId", s.gClientId);
    str("gFolderName", s.gFolderName);
    str("gFolderId", s.gFolderId);
    str("gAccount", s.gAccount);
    num("bandwidthMbps", s.bandwidthMbps);
    bol("pauseWhileStreaming", s.pauseWhileStreaming);
    bol("upProgram", s.upProgram);
    bol("upIsoVideo", s.upIsoVideo);
    bol("upIsoAudio", s.upIsoAudio);
    bol("deleteAfterUpload", s.deleteAfterUpload);
    bol("watchFolder", s.watchFolder);
    num("stableSeconds", s.stableSeconds);
    return hb::core::json::dump(o, true);
}

bool settingsFromJson(const std::string& json, CloudSettings& out) {
    Json r;
    if (!hb::core::json::parse(json, r) || !r.isObj()) return false;
    CloudSettings s = out;
    auto str = [&](const char* k, std::string& dst) { if (auto* v = r.get(k); v && v->isStr()) dst = v->s; };
    auto num = [&](const char* k, double& dst) { if (auto* v = r.get(k); v && v->isNum()) dst = v->n; };
    auto inum = [&](const char* k, int& dst) { if (auto* v = r.get(k); v && v->isNum()) dst = static_cast<int>(std::clamp(v->n, -1e9, 1e9)); };
    auto bol = [&](const char* k, bool& dst) { if (auto* v = r.get(k); v && v->isBool()) dst = v->b; };
    bol("enabled", s.enabled);
    str("provider", s.provider);
    str("s3Endpoint", s.s3Endpoint);
    str("s3Region", s.s3Region);
    str("s3Bucket", s.s3Bucket);
    str("s3Prefix", s.s3Prefix);
    str("s3Addressing", s.s3Addressing);
    inum("partSizeMiB", s.partSizeMiB);
    str("gClientId", s.gClientId);
    str("gFolderName", s.gFolderName);
    str("gFolderId", s.gFolderId);
    str("gAccount", s.gAccount);
    num("bandwidthMbps", s.bandwidthMbps);
    bol("pauseWhileStreaming", s.pauseWhileStreaming);
    bol("upProgram", s.upProgram);
    bol("upIsoVideo", s.upIsoVideo);
    bol("upIsoAudio", s.upIsoAudio);
    bol("deleteAfterUpload", s.deleteAfterUpload);
    bol("watchFolder", s.watchFolder);
    inum("stableSeconds", s.stableSeconds);
    out = sanitize(s);
    return true;
}

bool loadSettings(const std::string& path, CloudSettings& out) {
    std::string text;
    {
        std::ifstream f(std::filesystem::path(std::u8string(path.begin(), path.end())), std::ios::binary);
        if (!f) return false;
        std::stringstream ss;
        ss << f.rdbuf();
        text = ss.str();
    }
    if (text.size() > 1024 * 1024) return false;
    return settingsFromJson(text, out);
}

bool saveSettings(const std::string& path, const CloudSettings& s) {
    const auto p = std::filesystem::path(std::u8string(path.begin(), path.end()));
    const std::string tmp = path + ".tmp";
    const auto tp = std::filesystem::path(std::u8string(tmp.begin(), tmp.end()));
    {
        std::ofstream f(tp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << settingsToJson(s);
        if (!f.good()) return false;
    }
    std::error_code ec;
    std::filesystem::rename(tp, p, ec);
    return !ec;
}

}  // namespace hb::cloud
