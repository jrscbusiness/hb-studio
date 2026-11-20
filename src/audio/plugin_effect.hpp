#pragma once
// Adapter between the C plugin ABI (hb_effects_v1, src/plugins/hb_plugin_api.h) and hb::audio::Effect.
// Everything that comes from a plugin is untrusted: it is validated here and every call into the plugin is
// guarded (exceptions, non-finite output).
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "audio/effect.hpp"
#include "plugins/hb_plugin_api.h"

namespace hb::audio {

struct PluginEffectLibrary {
    hb_effects_v1 api{};
    std::shared_ptr<void> keepAlive;   // keeps the shared library loaded while any effect exists
};

struct PluginEffectDesc {
    std::string typeId, name;
    std::vector<EffectParamInfo> params;
};

inline constexpr int kPluginMaxEffects = 64;
inline constexpr int kPluginMaxConsecutiveBadBlocks = 8;   // then the wrapper bypasses the plugin

// Calls count()/describe() and validates everything: required function pointers, count <= 64, params <= 32,
// NUL-terminated strings within their buffers, type_id [A-Za-z0-9._-], param id [a-z0-9_] and unique,
// finite min < max, default inside the range. All-or-nothing: on any problem returns false and fills `error`.
bool describePluginEffects(const PluginEffectLibrary& lib, std::vector<PluginEffectDesc>& out, std::string& error);

// Effect backed by a plugin instance. The instance is created in prepare() (it needs rate/channels) and is
// only ever touched from the thread that calls prepare()/process()/reset(); setParam() only stores atomics
// that process() forwards to the plugin. Before the first prepare() process() is a no-op.
class PluginEffect final : public Effect {
public:
    PluginEffect(std::shared_ptr<PluginEffectLibrary> lib, int index, PluginEffectDesc desc);
    ~PluginEffect() override;

    const char* typeId() const override { return desc_.typeId.c_str(); }
    const std::vector<EffectParamInfo>& params() const override { return desc_.params; }
    void setParam(const std::string& id, float value) override;
    float getParam(const std::string& id) const override;
    void prepare(int sampleRate, int channels) override;
    void process(float* interleaved, std::size_t frames) override;
    void reset() override;

    std::uint64_t errorCount() const noexcept { return errors_.load(std::memory_order_relaxed); }
    bool bypassed() const noexcept { return bypass_.load(std::memory_order_relaxed); }

private:
    void destroyInstance() noexcept;
    int indexOf(const std::string& id) const noexcept;

    std::shared_ptr<PluginEffectLibrary> lib_;
    int index_;
    PluginEffectDesc desc_;
    std::vector<std::atomic<float>> values_;
    std::vector<float> pushed_;          // last value forwarded to the plugin (audio thread only)
    void* inst_ = nullptr;
    int channels_ = 2;
    int consecutiveBad_ = 0;
    std::atomic<std::uint64_t> errors_{0};
    std::atomic<bool> bypass_{false};
};

std::unique_ptr<Effect> createPluginEffect(std::shared_ptr<PluginEffectLibrary> lib, int index,
                                           const PluginEffectDesc& desc);

}  // namespace hb::audio
