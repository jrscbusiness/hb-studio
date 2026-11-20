#pragma once
// Contract shared by the DSP module, the audio engine and the plugin loader.
// FROZEN INTERFACE: the three modules are written in parallel against this header.
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace hb::audio {

struct EffectParamInfo {
    std::string id;       // [a-z0-9_]
    std::string label;    // pt-BR, shown to the operator
    std::string unit;     // "dB", "Hz", "ms", "%", ""
    float min = 0, max = 1, def = 0;
    bool logarithmic = false;            // slider scale (frequencies)
    std::vector<std::string> choices;    // non-empty: the value is an index into this list
};

// One audio effect instance on one channel strip.
//   - process() is real-time: no allocation, no locks, no blocking, no exceptions.
//   - setParam()/getParam() may be called from any thread while process() runs; implementations
//     must smooth changes (no zipper noise) and use atomics (no locks).
//   - prepare() is NOT real-time; call it before process() and whenever rate/channels change.
class Effect {
public:
    virtual ~Effect() = default;
    virtual const char* typeId() const = 0;                          // e.g. "hb.compressor"
    virtual const std::vector<EffectParamInfo>& params() const = 0;
    virtual void setParam(const std::string& id, float value) = 0;   // unknown id / out of range: ignored / clamped
    virtual float getParam(const std::string& id) const = 0;
    virtual void prepare(int sampleRate, int channels) = 0;
    virtual void process(float* interleaved, std::size_t frames) = 0;   // in place
    virtual void reset() = 0;
    virtual float gainReductionDb() const { return 0.f; }            // >= 0, for meters (compressor/gate/limiter)
};

// Process-wide factory table: built-in effects and effects from installed plugins register here.
class EffectRegistry {
public:
    using Factory = std::function<std::unique_ptr<Effect>()>;
    struct Info {
        std::string typeId;
        std::string name;        // shown in the "add effect" menu
        std::string category;    // "Dinâmica", "Equalização", "Utilitários", "Plugin"
        std::string source;      // "builtin" or the plugin id
    };

    static EffectRegistry& instance();
    bool add(const Info& info, Factory factory);          // false: empty/duplicate typeId
    void removeBySource(const std::string& source);       // e.g. when a plugin is disabled
    std::unique_ptr<Effect> create(const std::string& typeId) const;   // nullptr if unknown
    std::vector<Info> list() const;
    bool contains(const std::string& typeId) const;

private:
    mutable std::mutex mutex_;
    std::map<std::string, std::pair<Info, Factory>> items_;
};

// Defined by the DSP module (src/audio/effects/): registers hb.highpass, hb.eq3, hb.gate, ...
void registerBuiltinEffects();

}  // namespace hb::audio
