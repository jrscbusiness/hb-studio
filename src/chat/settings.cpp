#include "chat/settings.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>

#include "core/json.hpp"

namespace hb::chat {

namespace {
using Json = hb::core::json::Value;
std::string strOf(const Json& o, const char* k, std::size_t max) {
    const Json* v = o.get(k);
    if (!v || !v->isStr()) return {};
    std::string s;
    for (unsigned char c : v->s) {
        if (c < 0x20 || c == 0x7F) continue;
        if (s.size() >= max) break;
        s += static_cast<char>(c);
    }
    return s;
}
bool allIn(const std::string& s, bool (*pred)(unsigned char)) {
    return std::all_of(s.begin(), s.end(), [&](char c) { return pred(static_cast<unsigned char>(c)); });
}
bool videoChar(unsigned char c) { return std::isalnum(c) || c == '_' || c == '-'; }
}  // namespace

std::string cleanVideoId(const std::string& in) {
    std::string s = in;
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(0, 1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    if (s.size() > 300) return "";
    auto grab = [&](const std::string& marker) -> std::string {
        const auto p = s.find(marker);
        if (p == std::string::npos) return "";
        std::string id;
        for (std::size_t i = p + marker.size(); i < s.size() && videoChar(static_cast<unsigned char>(s[i])); ++i) id += s[i];
        return id;
    };
    std::string id;
    if (s.find("://") != std::string::npos || s.find("youtu") != std::string::npos) {
        id = grab("v=");
        if (id.empty()) id = grab("youtu.be/");
        if (id.empty()) id = grab("/live/");
        if (id.empty()) id = grab("/embed/");
    } else {
        id = s;
    }
    if (id.empty() || id.size() > 32 || !allIn(id, videoChar)) return "";
    return id;
}

std::string cleanGraphId(const std::string& in) {
    std::string s;
    for (char c : in)
        if (!std::isspace(static_cast<unsigned char>(c))) s += c;
    if (s.empty() || s.size() > 40) return "";
    if (!allIn(s, [](unsigned char c) { return std::isdigit(c) || c == '_'; })) return "";
    return s;
}

std::string cleanTwitchChannel(const std::string& in) {
    std::string s;
    for (char c : in) s += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(0, 1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    if (!s.empty() && s[0] == '#') s.erase(0, 1);
    const auto p = s.find("twitch.tv/");   // accept a pasted channel URL
    if (p != std::string::npos) s = s.substr(p + 10);
    while (!s.empty() && s.back() == '/') s.pop_back();
    if (s.empty() || s.size() > 25) return "";
    if (!allIn(s, [](unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; })) return "";
    return s;
}

std::string cleanClientId(const std::string& in) {
    std::string s;
    for (char c : in)
        if (!std::isspace(static_cast<unsigned char>(c))) s += c;
    if (s.empty() || s.size() > 200) return "";
    if (!allIn(s, [](unsigned char c) { return std::isalnum(c) || c == '-' || c == '.' || c == '_'; })) return "";
    return s;
}

std::string ChatSettings::toJson() const {
    std::ostringstream o;
    auto e = [](const std::string& s) { return hb::core::json::escape(s); };
    o << "{\"schema\":1,\"youtubeVideoId\":\"" << e(youtubeVideoId) << "\",\"youtubeClientId\":\"" << e(youtubeClientId)
      << "\",\"facebookLiveVideoId\":\"" << e(facebookLiveVideoId) << "\",\"twitchChannel\":\"" << e(twitchChannel)
      << "\",\"persistMessages\":" << (persistMessages ? "true" : "false") << ",\"muted\":[";
    for (std::size_t i = 0; i < muted.size(); ++i) o << (i ? "," : "") << "\"" << e(muted[i]) << "\"";
    o << "]}";
    return o.str();
}

bool ChatSettings::fromJson(const std::string& json, ChatSettings& out) {
    Json root;
    if (!hb::core::json::parse(json, root) || !root.isObj()) return false;
    ChatSettings s;
    s.youtubeVideoId = cleanVideoId(strOf(root, "youtubeVideoId", 64));
    s.youtubeClientId = cleanClientId(strOf(root, "youtubeClientId", 200));
    s.facebookLiveVideoId = cleanGraphId(strOf(root, "facebookLiveVideoId", 64));
    s.twitchChannel = cleanTwitchChannel(strOf(root, "twitchChannel", 40));
    if (const Json* v = root.get("persistMessages"); v && v->isBool()) s.persistMessages = v->b;
    if (const Json* m = root.get("muted"); m && m->isArr())
        for (const Json& k : m->a)
            if (k.isStr() && !k.s.empty() && k.s.size() <= 160 && s.muted.size() < 500) s.muted.push_back(k.s);
    out = std::move(s);
    return true;
}

bool ChatSettings::load(const std::string& path) {
    std::string text;
    {
        std::ifstream f(path, std::ios::binary);
        if (!f) return false;
        std::stringstream ss;
        ss << f.rdbuf();
        text = ss.str();
    }
    if (text.size() > 1024 * 1024) return false;
    return fromJson(text, *this);
}

bool ChatSettings::save(const std::string& path) const {
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

}  // namespace hb::chat
