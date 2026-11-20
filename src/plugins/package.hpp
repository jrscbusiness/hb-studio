#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "plugins/manifest.hpp"

namespace hb::plugins {

// Limits applied to every .hbplugin package (zip). A hostile package must not be able to exhaust
// disk/memory ("zip bomb"), write outside its folder ("zip slip"), or smuggle unchecked executables.
inline constexpr std::uint64_t kMaxPackageBytes = 64ull * 1024 * 1024;
inline constexpr std::uint64_t kMaxEntryBytes = 64ull * 1024 * 1024;
inline constexpr std::uint64_t kMaxTotalBytes = 128ull * 1024 * 1024;
inline constexpr std::size_t kMaxEntries = 200;
inline constexpr std::size_t kMaxManifestBytes = 64 * 1024;

struct PackageInfo {
    Manifest manifest;
    std::string packageSha256;        // of the zip file itself: binds "what the user reviewed" to "what gets installed"
    std::uint64_t totalSize = 0;      // uncompressed
    int fileCount = 0;
    bool supportedHere = true;        // false: native plugin without a library for this platform
    std::vector<std::string> files;
};

struct PackageResult {
    bool ok = false;
    std::string error;                // pt-BR, safe to show
    PackageInfo info;
};

bool readFileBytes(const std::string& pathUtf8, std::uint64_t maxBytes, std::vector<std::uint8_t>& out, std::string& err);

// Validates structure, limits, names, manifest and the sha256 of every file listed in it.
PackageResult inspectPackageBytes(const std::vector<std::uint8_t>& zip, const std::string& platformKey);
PackageResult inspectPackage(const std::string& zipPathUtf8, const std::string& platformKey);

// Re-validates `zip`, requires its sha256 to equal `expectedPackageSha` (empty = skip), then writes
// every file under `destDir` (created; must not already contain files). Files are written from the
// same in-memory bytes that were validated.
bool extractPackageBytes(const std::vector<std::uint8_t>& zip, const std::string& platformKey,
                         const std::string& expectedPackageSha, const std::string& destDirUtf8, PackageInfo& info,
                         std::string& err);

}  // namespace hb::plugins
