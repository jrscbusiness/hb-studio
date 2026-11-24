#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Marketplace catalog: a static index.json (schemaVersion 1) hosted in a public Git repository.
// Everything in it is UNTRUSTED input. The parser is strict (types, lengths, charsets, https-only
// URLs, lowercase-hex hashes, size caps), drops any entry that does not validate (with a warning) and
// never throws or crashes. The schema is documented in docs/MARKETPLACE.md and marketplace/index.schema.json
// and must be kept in sync with the Python/Node validator in marketplace/validate_index.py.
namespace hb::marketplace {

inline constexpr int kSupportedSchemaVersion = 1;
inline constexpr std::size_t kMaxCatalogBytes = 2 * 1024 * 1024;     // size of index.json
inline constexpr std::size_t kMaxCatalogEntries = 500;
inline constexpr std::uint64_t kMaxPackageBytes = 64ull * 1024 * 1024;   // same cap as the plugin package reader

enum class Category { Theme, VideoSource, AudioEffect, Graphic, Template };
enum class PackageType { Theme, Source, Effect };   // what the .hbplugin manifest "type" will say

struct VersionEntry {
    std::string version;
    std::string minAppVersion = "0.0.0";
    std::vector<std::string> platforms;     // e.g. "windows-x64"
    std::vector<std::string> downloadUrls;  // https only, tried in order (mirrors)
    std::string sha256;                     // of the .hbplugin file, 64 lowercase hex
    std::uint64_t size = 0;                 // bytes of the .hbplugin file
    std::string releasedAt, notes;
};

struct CatalogEntry {
    std::string id, name, author;
    std::string descriptionPt, descriptionEn;
    Category category = Category::Theme;
    PackageType type = PackageType::Theme;
    std::vector<std::string> tags;
    std::string license, homepage, repository;
    bool verified = false;                  // set by maintainers only (reviewed in the catalog repository)
    std::vector<std::string> screenshots;   // https URLs, loaded lazily by the UI
    std::vector<std::string> permissions;   // "native-code", "network", "filesystem", "hardware-capture"
    std::vector<VersionEntry> versions;

    // Risk model shared with the plugin system: themes never execute code, everything else is native code.
    bool native() const { return type != PackageType::Theme; }
};

struct Catalog {
    int schemaVersion = 0;
    std::string generatedAt;
    std::vector<CatalogEntry> entries;
    std::vector<std::string> warnings;      // one per dropped entry / ignored problem (pt-BR, capped)
    std::size_t dropped = 0;                // entries that failed validation
};

struct ParseResult {
    bool ok = false;
    std::string error;                      // pt-BR, short
    Catalog catalog;
};

ParseResult parseCatalog(std::string_view text);

// ---- validators (exposed for tests) ----
bool isSafeHttpsUrl(std::string_view url);           // https only, no userinfo, no IP literal/localhost, printable ASCII
bool isValidUtf8(std::string_view s);
bool validCatalogId(std::string_view id);             // same rules as the plugin manifest id
const char* categoryName(Category c);                 // "theme", "video-source", ...
const char* packageTypeName(PackageType t);           // "theme", "source", "effect"

// ---- version selection / update logic ----
struct Selection {
    const VersionEntry* version = nullptr;   // highest compatible stable version, or null
    std::string reason;                      // pt-BR, why there is none (only when version == nullptr)
};
// Highest non-prerelease version that lists `platform` and whose minAppVersion <= appVersion.
Selection selectVersion(const CatalogEntry& e, std::string_view appVersion, std::string_view platform);

enum class InstallStatus { Available, Installed, UpdateAvailable };
// installedVersion empty = not installed.
InstallStatus installStatus(const Selection& sel, std::string_view installedVersion);
const char* installStatusName(InstallStatus s);       // "available", "installed", "update"

}  // namespace hb::marketplace
