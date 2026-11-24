#include "captions/settings.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>

#include "captions/cloud_recognizer.hpp"
#include "captions/model_manager.hpp"
#include "core/json.hpp"

namespace hb::captions {
namespace {
const char* const kLangs[] = {"pt", "en", "es", "fr", "de", "it", "auto"};

bool parseBool(const std::string& v, bool& out) {
    if (v == "1" || v == "true") { out = true; return true; }
    if (v == "0" || v == "false") { out = false; return true; }
    return false;
}
bool parseInt(const std::string& v, int lo, int hi, int& out) {
    if (v.empty() || v.size() > 6) return false;
    char* end = nullptr;
    const long n = std::strtol(v.c_str(), &end, 10);
    if (*end != '\0' || n < lo || n > hi) return false;
    out = static_cast<int>(n);
    return true;
}
bool plainText(const std::string& v, std::size_t maxLen) {
    if (v.size() > maxLen) return false;
    for (unsigned char c : v)
        if (c < 0x20 || c == 0x7F) return false;
    return true;
}
}  // namespace

bool isSupportedLanguage(const std::string& code) {
    for (const char* l : kLangs)
        if (code == l) return true;
    return false;
}

const char* const* supportedLanguages(std::size_t& count) {
    count = sizeof kLangs / sizeof kLangs[0];
    return kLangs;
}

bool applySetting(CaptionSettings& s, const std::string& key, const std::string& value) {
    CaptionSettings t = s;
    if (key == "engine") {
        if (value != "whisper" && value != "cloud") return false;
        t.engine = value;
    } else if (key == "model") {
        if (!findModel(modelCatalog(), value)) return false;
        t.model = value;
    } else if (key == "language") {
        if (!isSupportedLanguage(value)) return false;
        t.language = value;
    } else if (key == "profanity") {
        if (!parseBool(value, t.profanityFilter)) return false;
    } else if (key == "saveSrt") {
        if (!parseBool(value, t.saveSrt)) return false;
    } else if (key == "maxChars") {
        if (!parseInt(value, 20, 80, t.maxCharsPerLine)) return false;
    } else if (key == "maxLines") {
        if (!parseInt(value, 1, 3, t.maxLines)) return false;
    } else if (key == "cloudUrl") {
        if (!value.empty() && !validateBaseUrl(value).empty()) return false;
        t.cloudBaseUrl = value;
    } else if (key == "cloudModel") {
        if (value.empty() || !plainText(value, 100)) return false;
        t.cloudModel = value;
    } else if (key == "cloudKey") {
        if (!plainText(value, 400)) return false;
        t.cloudKey = value;   // empty clears it
    } else {
        return false;
    }
    s = std::move(t);
    return true;
}

std::string settingsToJson(const CaptionSettings& s) {
    using namespace hb::core::json;
    Value o;
    o.t = Value::T::Obj;
    auto str = [](const std::string& x) { Value v; v.t = Value::T::Str; v.s = x; return v; };
    auto num = [](double x) { Value v; v.t = Value::T::Num; v.n = x; return v; };
    auto boo = [](bool x) { Value v; v.t = Value::T::Bool; v.b = x; return v; };
    o.o["engine"] = str(s.engine);
    o.o["model"] = str(s.model);
    o.o["language"] = str(s.language);
    o.o["profanity"] = boo(s.profanityFilter);
    o.o["saveSrt"] = boo(s.saveSrt);
    o.o["maxChars"] = num(s.maxCharsPerLine);
    o.o["maxLines"] = num(s.maxLines);
    o.o["cloudUrl"] = str(s.cloudBaseUrl);
    o.o["cloudModel"] = str(s.cloudModel);
    o.o["cloudKey"] = str(s.cloudKey);
    return dump(o, true);
}

bool settingsFromJson(const std::string& json, CaptionSettings& out) {
    using namespace hb::core::json;
    Value v;
    if (!parse(json, v, Limits{64 * 1024, 4, 64}) || !v.isObj()) return false;
    CaptionSettings s;   // defaults, then each valid field
    auto tryStr = [&](const char* k) {
        const Value* x = v.get(k);
        if (x && x->isStr()) applySetting(s, k, x->s);
    };
    auto tryBoolNum = [&](const char* k) {
        const Value* x = v.get(k);
        if (!x) return;
        if (x->isBool()) applySetting(s, k, x->b ? "1" : "0");
        else if (x->isNum()) applySetting(s, k, std::to_string(static_cast<long>(x->n)));
    };
    for (const char* k : {"engine", "model", "language", "cloudUrl", "cloudModel", "cloudKey"}) tryStr(k);
    for (const char* k : {"profanity", "saveSrt", "maxChars", "maxLines"}) tryBoolNum(k);
    out = std::move(s);
    return true;
}

bool loadSettings(const std::string& pathUtf8, CaptionSettings& out) {
    out = CaptionSettings{};
    if (pathUtf8.empty()) return true;
    try {
        std::ifstream f(std::filesystem::path(reinterpret_cast<const char8_t*>(pathUtf8.c_str())), std::ios::binary);
        if (!f) return true;   // first run
        std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (!settingsFromJson(text, out)) out = CaptionSettings{};
        return true;
    } catch (...) {
        out = CaptionSettings{};
        return true;
    }
}

bool saveSettings(const std::string& pathUtf8, const CaptionSettings& s) {
    if (pathUtf8.empty()) return false;
    try {
        const std::filesystem::path p(reinterpret_cast<const char8_t*>(pathUtf8.c_str()));
        std::error_code ec;
        if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
        std::ofstream f(p, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << settingsToJson(s);
        return static_cast<bool>(f);
    } catch (...) {
        return false;
    }
}

}  // namespace hb::captions
