#pragma once
#include <filesystem>
#include <string>

namespace hb::plugins {

// All paths in the plugin module are UTF-8 std::string (the app talks UTF-8 everywhere);
// std::filesystem is used for the actual I/O so non-ASCII Windows user names work.
inline std::filesystem::path pathFromUtf8(const std::string& s) {
    return std::filesystem::path(std::u8string(s.begin(), s.end()));
}
inline std::string pathToUtf8(const std::filesystem::path& p) {
    const auto u = p.u8string();
    return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

}  // namespace hb::plugins
