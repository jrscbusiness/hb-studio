#pragma once
#include <chrono>
#include <map>
#include <mutex>
#include <string>

#include "plugins/manager.hpp"
#include "plugins/runtime.hpp"

namespace hb::plugins {

// Everything the UI needs from the plugin system, as JSON strings, plus the two-step install flow:
//   beginInstall(file)   inspects the package, shows the user what it contains (never installs)
//   confirmInstall(tok)  installs exactly the package that was reviewed (its sha256 is bound to the token)
class PluginService {
public:
    PluginService(std::string rootDirUtf8, std::string platformKey);

    PluginManager& manager() { return manager_; }
    PluginRuntime& runtime() { return runtime_; }

    void start();                                   // scan folder, load enabled native plugins

    std::string beginInstall(const std::string& packagePath);       // -> {"t":"plugin_review",...}
    std::string confirmInstall(const std::string& token);           // -> {"t":"plugin_result",...}
    void cancelInstall(const std::string& token);

    Result setEnabled(const std::string& id, bool on);              // also loads native libraries
    Result remove(const std::string& id);

    std::string listJson() const;                                    // {"t":"plugins","ver":N,"list":[...]}
    std::string themeJson() const;                                   // {"t":"theme","id":"...","scheme":"dark","vars":{...}}

    // Test hook: make pending reviews expire quickly.
    void setReviewTtl(std::chrono::seconds ttl) { ttl_ = ttl; }

private:
    struct Pending {
        std::string path, sha;
        std::chrono::steady_clock::time_point expires;
    };
    static std::string resultJson(bool ok, const std::string& message, const std::string& id = "");

    PluginManager manager_;
    PluginRuntime runtime_;
    mutable std::mutex mutex_;
    std::map<std::string, Pending> pending_;
    std::chrono::seconds ttl_{600};
};

}  // namespace hb::plugins
