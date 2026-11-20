#include "plugins/runtime.hpp"

namespace hb::plugins {

bool PluginRuntime::parseFullId(const std::string& fullId, std::string& pluginId, std::string& sourceId) {
    if (fullId.rfind("plugin:", 0) != 0) return false;
    const auto second = fullId.find(':', 7);
    if (second == std::string::npos) return false;
    pluginId = fullId.substr(7, second - 7);
    sourceId = fullId.substr(second + 1);
    return validPluginId(pluginId) && NativePlugin::validSourceId(sourceId);
}

void PluginRuntime::sync() {
    for (const auto& id : manager_.enabledNativeIds()) {
        {
            std::lock_guard lock(mutex_);
            if (libs_.count(id)) continue;
        }
        std::string path, err;
        if (!manager_.verifiedLibraryPath(id, path, err)) {   // hash mismatch etc.: never load it
            manager_.setRuntimeError(id, err);
            continue;
        }
        auto lib = NativePlugin::load(path, id, err);
        if (!lib) {
            manager_.setRuntimeError(id, "falha ao carregar: " + err);
            continue;
        }
        {
            std::lock_guard lock(mutex_);
            libs_[id] = lib;
        }
        manager_.markLoaded(id, true);
    }
}

std::shared_ptr<NativePlugin> PluginRuntime::loaded(const std::string& pluginId) const {
    InstalledPlugin ip;
    if (!manager_.find(pluginId, ip) || !ip.enabled) return nullptr;
    std::lock_guard lock(mutex_);
    auto it = libs_.find(pluginId);
    return it == libs_.end() ? nullptr : it->second;
}

std::vector<PluginRuntime::SourceEntry> PluginRuntime::sources() {
    std::vector<SourceEntry> out;
    for (const auto& ip : manager_.list()) {
        if (!ip.enabled || ip.manifest.type != PluginType::Source) continue;
        auto lib = loaded(ip.manifest.id);
        if (!lib || !lib->hasSources()) continue;
        std::vector<NativePlugin::SourceInfo> list;
        std::string err;
        if (!lib->listSources(list, err)) {
            manager_.setRuntimeError(ip.manifest.id, err);
            continue;
        }
        for (const auto& s : list)
            out.push_back({makeFullId(ip.manifest.id, s.id), s.name, ip.manifest.id, ip.manifest.name});
    }
    return out;
}

std::shared_ptr<video::LiveSource> PluginRuntime::open(const std::string& fullId, std::string& error) {
    std::string pid, sid;
    if (!parseFullId(fullId, pid, sid)) {
        error = "identificador de fonte de plugin inválido";
        return nullptr;
    }
    auto lib = loaded(pid);
    if (!lib) {
        error = "o plugin não está ativo";
        return nullptr;
    }
    return lib->open(sid, error);
}

}  // namespace hb::plugins
