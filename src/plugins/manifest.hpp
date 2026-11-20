#pragma once
#include <map>
#include <string>
#include <string_view>

namespace hb::plugins {

enum class PluginType { Source, Effect, Theme };

// manifest.json at the root of a .hbplugin package (a zip file).
//   { "schema": 1, "id": "com.example.bars", "name": "...", "version": "1.0.0", "author": "...",
//     "description": "...", "license": "MIT",
//     "type": "source", "apiVersion": 1,
//       "library": {"windows-x64": "plugin.dll", "linux-x64": "libplugin.so"},
//       "sha256":  {"plugin.dll": "<64 hex>", "libplugin.so": "<64 hex>"}
//     -- or --
//     "type": "effect"  (same fields as "source": audio effects, see hb_plugin_api.h)
//     -- or --
//     "type": "theme", "theme": {"colorScheme": "dark", "vars": {"--bg": "#0B1220", ...}} }
struct Manifest {
    std::string id, name, version, author, description, license;
    PluginType type = PluginType::Theme;
    int apiVersion = 0;
    std::map<std::string, std::string> libraries;   // platform key -> file name
    std::map<std::string, std::string> sha256;      // file name -> lowercase hex
    std::map<std::string, std::string> themeVars;   // sanitised CSS custom properties
    std::string colorScheme;                        // "light" | "dark" | ""

    // File name of the native library for `platformKey`, empty if this plugin has none for it.
    std::string libraryFor(const std::string& platformKey) const {
        auto it = libraries.find(platformKey);
        return it == libraries.end() ? std::string() : it->second;
    }
};

struct ManifestResult {
    bool ok = false;
    std::string error;   // human readable (pt-BR), never contains raw plugin text longer than 80 chars
    Manifest manifest;
};

ManifestResult parseManifest(std::string_view json);

// Validators (exposed for tests and for the package reader).
bool validPluginId(const std::string& id);
bool validVersion(const std::string& v);
bool validLibraryFileName(const std::string& f);
bool validSha256Hex(const std::string& h);
// A single path component / archive entry name is safe to create on Windows and POSIX.
bool safeEntryName(const std::string& name, int maxDepth = 4);

// Theme: only whitelisted custom properties with colour values are accepted.
bool validThemeVarName(const std::string& n);
bool validThemeColor(const std::string& v);

}  // namespace hb::plugins
