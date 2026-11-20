#include "recording/file_naming.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>

#include "recording/file_io.hpp"

namespace hb::rec {

namespace {

bool isReservedDeviceName(const std::string& base) {
    std::string up;
    for (char c : base) up.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    if (up == "CON" || up == "PRN" || up == "AUX" || up == "NUL") return true;
    if (up.size() == 4 && (up.compare(0, 3, "COM") == 0 || up.compare(0, 3, "LPT") == 0) && up[3] >= '1' && up[3] <= '9')
        return true;
    return false;
}

void replaceAll(std::string& s, const std::string& from, const std::string& to) {
    std::size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
}

}  // namespace

std::string sanitizeFileName(const std::string& name) {
    std::string s;
    for (unsigned char c : name) {
        if (c < 0x20 || c == 0x7F) continue;
        switch (c) {
            case '<': case '>': case ':': case '"': case '/': case '\\': case '|': case '?': case '*':
                s.push_back('_');
                break;
            default:
                s.push_back(static_cast<char>(c));
        }
    }
    while (s.find("..") != std::string::npos) replaceAll(s, "..", "_");
    while (!s.empty() && (s.back() == '.' || s.back() == ' ')) s.pop_back();
    std::size_t lead = 0;
    while (lead < s.size() && (s[lead] == '.' || s[lead] == ' ')) ++lead;
    s.erase(0, lead);
    if (s.size() > 120) {
        std::size_t cut = 120;
        while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;  // do not split UTF-8
        s.resize(cut);
        while (!s.empty() && (s.back() == '.' || s.back() == ' ')) s.pop_back();
    }
    if (s.empty()) return "HBStudio";
    const std::string base = s.substr(0, s.find('.'));
    if (isReservedDeviceName(base)) s.insert(s.begin(), '_');
    return s;
}

std::string expandNameTemplate(const std::string& tmpl, const std::tm& t, int n) {
    char date[32], time[32];
    std::snprintf(date, sizeof(date), "%04d-%02d-%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
    std::snprintf(time, sizeof(time), "%02d-%02d-%02d", t.tm_hour, t.tm_min, t.tm_sec);
    std::string s = tmpl;
    replaceAll(s, "{date}", date);
    replaceAll(s, "{time}", time);
    replaceAll(s, "{n}", std::to_string(n));
    return s;
}

bool pathExists(const std::string& pathUtf8) {
    std::error_code ec;
    const bool e = std::filesystem::exists(detail::pathFromUtf8(pathUtf8), ec);
    return e || ec;  // unknown -> assume it exists (safer than overwriting)
}

std::string uniqueOutputPath(const std::string& folderUtf8, const std::string& name, const std::string& ext) {
    const auto dir = detail::pathFromUtf8(folderUtf8);
    const std::string e = ext.empty() ? "" : "." + ext;
    for (int i = 1; i < 100000; ++i) {
        const std::string leaf = (i == 1 ? name : name + " (" + std::to_string(i) + ")") + e;
        const std::string p = detail::pathToUtf8(dir / detail::pathFromUtf8(leaf));
        if (!pathExists(p)) return p;
    }
    return detail::pathToUtf8(dir / detail::pathFromUtf8(name + "_" + std::to_string(std::rand()) + e));
}

std::string firstFreeName(const std::string& folderUtf8, const std::string& tmpl, const std::tm& local,
                          const std::string& ext) {
    if (tmpl.find("{n}") == std::string::npos) return sanitizeFileName(expandNameTemplate(tmpl, local, 1));
    const auto dir = detail::pathFromUtf8(folderUtf8);
    const std::string e = ext.empty() ? "" : "." + ext;
    std::string name;
    for (int n = 1; n < 100000; ++n) {
        name = sanitizeFileName(expandNameTemplate(tmpl, local, n));
        const std::string p = detail::pathToUtf8(dir / detail::pathFromUtf8(name + e));
        if (!pathExists(p)) return name;
    }
    return name;
}

}  // namespace hb::rec
