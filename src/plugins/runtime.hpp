#pragma once
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "plugins/manager.hpp"
#include "plugins/native_source.hpp"

namespace hb::plugins {

// Connects the on-disk registry (PluginManager) with loaded native libraries.
//   sync():    loads libraries of enabled plugins (after re-hashing them) and reports failures to the manager.
//   sources(): video sources offered by enabled+loaded source plugins
//   open():    "plugin:<pluginId>:<sourceId>" -> live source
class PluginRuntime {
public:
    explicit PluginRuntime(PluginManager& manager) : manager_(manager) {}

    struct SourceEntry {
        std::string fullId, name, pluginId, pluginName;
    };

    void sync();
    std::vector<SourceEntry> sources();
    std::shared_ptr<video::LiveSource> open(const std::string& fullId, std::string& error);
    // Loaded library of an enabled plugin (effects loader uses it).
    std::shared_ptr<NativePlugin> loaded(const std::string& pluginId) const;

    static bool parseFullId(const std::string& fullId, std::string& pluginId, std::string& sourceId);
    static std::string makeFullId(const std::string& pluginId, const std::string& sourceId) {
        return "plugin:" + pluginId + ":" + sourceId;
    }

private:
    PluginManager& manager_;
    mutable std::mutex mutex_;
    std::map<std::string, std::shared_ptr<NativePlugin>> libs_;
};

}  // namespace hb::plugins
