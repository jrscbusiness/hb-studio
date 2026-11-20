#pragma once
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "plugins/manifest.hpp"
#include "plugins/package.hpp"

namespace hb::plugins {

struct InstalledPlugin {
    Manifest manifest;
    std::string dir;              // absolute, UTF-8
    bool valid = true;            // manifest readable and consistent with the folder
    std::string error;            // why it is not valid / why it failed to load
    bool supported = true;        // has a library for this platform (source plugins)
    bool enabled = false;
    bool loaded = false;          // native library is in this process
    bool pendingRemoval = false;  // removed, files go away at the next start
};

struct Result {
    bool ok = false;
    std::string message;          // pt-BR
};

struct Review {
    bool ok = false;
    std::string error;
    PackageInfo info;
    bool replaces = false;
    std::string installedVersion;
};

// Installed-plugin registry on disk:   <root>/<id>/...   <root>/state.json (enabled ids)
// Installing never enables: a new plugin (and every update) starts disabled and needs an explicit
// user action. Native libraries are re-hashed against the manifest every time they are enabled/loaded.
class PluginManager {
public:
    PluginManager(std::string rootDirUtf8, std::string platformKey);

    void scan();                                     // (re)reads the folder; finishes pending removals
    std::vector<InstalledPlugin> list() const;
    bool find(const std::string& id, InstalledPlugin& out) const;

    Review inspect(const std::string& packagePath) const;
    Result install(const std::string& packagePath, const std::string& expectedPackageSha);
    Result setEnabled(const std::string& id, bool on);
    Result remove(const std::string& id);

    // Theme currently active (at most one enabled).
    std::map<std::string, std::string> activeThemeVars() const;
    std::string activeColorScheme() const;
    std::string activeThemeId() const;

    // Native: verified path of the library of an enabled plugin (re-hashes the file now).
    bool verifiedLibraryPath(const std::string& id, std::string& pathOut, std::string& err) const;
    std::vector<std::string> enabledNativeIds() const;

    // Runtime bookkeeping reported by the host.
    void markLoaded(const std::string& id, bool loaded);
    void setRuntimeError(const std::string& id, const std::string& message);

    unsigned version() const;                        // bumps on every change (UI refresh)
    const std::string& rootDir() const { return root_; }
    const std::string& platform() const { return platform_; }

private:
    void saveStateLocked() const;
    void loadStateLocked();
    InstalledPlugin* findLocked(const std::string& id);
    const InstalledPlugin* findLocked(const std::string& id) const;

    std::string root_, platform_;
    mutable std::mutex mutex_;
    std::vector<InstalledPlugin> items_;
    std::set<std::string> enabled_;
    unsigned version_ = 1;
};

}  // namespace hb::plugins
