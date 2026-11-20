#pragma once
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "plugins/hb_plugin_api.h"
#include "plugins/shared_library.hpp"
#include "video/source.hpp"

namespace hb::plugins {

// Protective limits for the real-time render path of a native source.
struct SourceLimits {
    double slowMs = 100.0;       // a render() slower than this counts as "slow"
    int maxSlowInARow = 20;      // ... this many in a row switch the source off
    int maxErrorsInARow = 30;    // render() < 0 this many times in a row switches the source off
};

// A loaded native plugin library. Validates everything the library hands us; calls into it are guarded
// (C++ exceptions are contained everywhere; access violations also on Windows via SEH).
class NativePlugin : public std::enable_shared_from_this<NativePlugin> {
public:
    struct SourceInfo {
        std::string id, name;
    };

    // Loads `libPath`. The library must export hb_plugin_get_v1 (source plugin) and/or
    // hb_plugin_get_effects_v1 (effect plugin).
    static std::shared_ptr<NativePlugin> load(const std::string& libPath, const std::string& pluginId, std::string& error);
    // Same validation for an API table that is already in memory (tests, statically linked plugins).
    static std::shared_ptr<NativePlugin> fromApi(const hb_plugin_v1& api, const std::string& pluginId, std::string& error);
    ~NativePlugin();

    const std::string& pluginId() const { return id_; }
    bool hasSources() const { return hasSource_; }
    bool hasEffects() const { return hasFx_; }

    bool listSources(std::vector<SourceInfo>& out, std::string& error);
    std::shared_ptr<video::LiveSource> open(const std::string& sourceId, std::string& error, SourceLimits limits = {});

    const hb_effects_v1& effectsApi() const { return fx_; }
    std::shared_ptr<SharedLibrary> library() const { return lib_; }

    static bool validSourceId(const std::string& id);

private:
    NativePlugin() = default;
    friend class PluginSource;
    std::string id_;
    std::shared_ptr<SharedLibrary> lib_;
    hb_plugin_v1 api_{};
    hb_effects_v1 fx_{};
    bool hasSource_ = false, hasFx_ = false;
    std::mutex listMutex_;
};

// Video source backed by a plugin's open source. Thread-safe; render() never throws.
class PluginSource final : public video::LiveSource {
public:
    PluginSource(std::shared_ptr<NativePlugin> plugin, void* handle, std::string name, SourceLimits limits);
    ~PluginSource() override;

    const std::string& name() const override { return name_; }
    void render(video::Frame& frame, std::uint64_t index) const override;
    video::SourceStatus status() const override;

private:
    std::shared_ptr<NativePlugin> plugin_;
    void* handle_;
    std::string name_;
    SourceLimits limits_;
    mutable std::mutex mutex_;
    mutable int slowRun_ = 0, errorRun_ = 0;
    mutable bool faulted_ = false;
    mutable std::string fault_;
    mutable bool noSignal_ = false, rendered_ = false;
    mutable std::chrono::steady_clock::time_point lastOk_{};
    mutable int w_ = 0, h_ = 0;
    mutable double fps_ = 0;
};

}  // namespace hb::plugins
