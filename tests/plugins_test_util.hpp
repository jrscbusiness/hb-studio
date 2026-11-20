#pragma once
#include <gtest/gtest.h>
#include <miniz.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "core/sha256.hpp"
#include "plugins/fs_util.hpp"

namespace hb::testutil {

using Entries = std::vector<std::pair<std::string, std::string>>;

#ifdef _WIN32
inline const char* platformKey() { return "windows-x64"; }
#else
inline const char* platformKey() { return "linux-x64"; }
#endif

// Builds a zip in memory. `compress` = false stores (needed to craft some hostile layouts).
inline std::vector<std::uint8_t> makeZip(const Entries& entries, bool compress = true) {
    mz_zip_archive z{};
    mz_zip_writer_init_heap(&z, 0, 1 << 16);
    for (const auto& [name, data] : entries)
        mz_zip_writer_add_mem(&z, name.c_str(), data.data(), data.size(), compress ? MZ_BEST_COMPRESSION : MZ_NO_COMPRESSION);
    void* buf = nullptr;
    size_t size = 0;
    mz_zip_writer_finalize_heap_archive(&z, &buf, &size);
    std::vector<std::uint8_t> out(static_cast<std::uint8_t*>(buf), static_cast<std::uint8_t*>(buf) + size);
    mz_zip_writer_end(&z);
    return out;
}

// The zip writer normalises some names (it drops a leading '/'). To craft hostile archives byte for byte,
// write the entry under a placeholder name of the same length and patch it in every header.
inline std::vector<std::uint8_t> makeZipRawName(const Entries& entries, const std::string& placeholder, const std::string& hostile) {
    auto zip = makeZip(entries, false);
    const std::size_t n = placeholder.size();
    for (std::size_t i = 0; i + n <= zip.size(); ++i)
        if (std::equal(placeholder.begin(), placeholder.end(), zip.begin() + static_cast<std::ptrdiff_t>(i)))
            std::copy(hostile.begin(), hostile.end(), zip.begin() + static_cast<std::ptrdiff_t>(i));
    return zip;
}

inline std::string themeManifest(const std::string& id = "com.test.theme", const std::string& extraVars = "") {
    return R"({"schema":1,"id":")" + id + R"(","name":"Tema de teste","version":"1.0.0","author":"t","type":"theme",
      "theme":{"colorScheme":"dark","vars":{"--bg":"#0A1226","--blue":"#4F8DFF")" + extraVars + R"(}}})";
}

inline std::string sourceManifest(const std::string& id, const std::string& libName, const std::string& libData,
                                  const std::string& otherPlatformHash = "") {
    (void)otherPlatformHash;
    const std::string sha = core::Sha256::hex(libData);
    return R"({"schema":1,"id":")" + id + R"(","name":"Fonte de teste","version":"1.2.3","type":"source","apiVersion":1,
      "library":{")" + platformKey() + R"(":")" + libName + R"("},"sha256":{")" + libName + R"(":")" + sha + R"("}})";
}

inline std::filesystem::path makeTempDir(const std::string& tag) {
    static std::atomic<int> n{0};
    auto p = std::filesystem::temp_directory_path() / ("hbtest_" + tag + "_" + std::to_string(::testing::UnitTest::GetInstance() ? 1 : 0) +
                                                       "_" + std::to_string(++n) + "_" + std::to_string(reinterpret_cast<std::uintptr_t>(&n) & 0xFFFFF));
    std::filesystem::remove_all(p);
    std::filesystem::create_directories(p);
    return p;
}

inline std::string pluginsPath(const std::filesystem::path& p) { return plugins::pathToUtf8(p); }
inline std::string pathToUtf8Join(const std::filesystem::path& base, const std::string& rel) {
    return plugins::pathToUtf8(base / plugins::pathFromUtf8(rel));
}

inline std::string writeFile(const std::filesystem::path& p, const std::vector<std::uint8_t>& bytes) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return plugins::pathToUtf8(p);
}

inline std::vector<std::uint8_t> readFile(const std::string& path) {
    std::ifstream f(plugins::pathFromUtf8(path), std::ios::binary);
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

}  // namespace hb::testutil
