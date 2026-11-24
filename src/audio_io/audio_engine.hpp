#pragma once
// HB Studio audio engine: up to 8 channel strips -> Program bus (+ headphone monitor).
// See docs/AUDIO-ENGINE.md.
//
// Threading model
//   * One engine thread produces a 480-frame (10 ms) block every 10 ms from a steady clock:
//     48 kHz, float32, stereo Program. It never locks, allocates or blocks: strips read their
//     parameters from atomics and their input / effect chain from immutable snapshots swapped
//     atomically (epoch based reclamation: old snapshots are freed by control threads only).
//   * Every public command is thread-safe, validated (NaN rejected, finite values clamped,
//     indices checked) and returns false with lastError() (pt-BR) on rejection. Never UB.
//   * Program consumers run on the engine thread: hand data off quickly.
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "audio/audio_common.hpp"
#include "audio/effect.hpp"
#include "audio/mixer.hpp"
#include "audio_io/device_enumerator.hpp"
#include "audio_io/device_stream.hpp"

namespace hb::audio_io {

inline constexpr int kMaxStrips = 8;
inline constexpr int kMaxEffects = 8;
inline constexpr std::size_t kBlockFrames = 480;  // 10 ms at 48 kHz

enum class InputKind { None, Device, External };
enum class Quality { Linear, Medium, High };
const char* qualityName(Quality q) noexcept;      // "linear" "medium" "high"
const char* inputKindName(InputKind k) noexcept;  // "none" "device" "external"

struct EngineOptions {
    int strips = kMaxStrips;                // 1..8
    Backend backend = Backend::Auto;
    DeviceOpener opener;                    // empty = miniaudio (tests inject fakes)
    DeviceEnumerator::Lister lister;        // empty = miniaudio
    std::function<std::int64_t()> clockNs;  // clock handed to the streams (tests); empty = steady clock
    bool streamSupervisors = true;          // false: runMaintenance() ticks the streams
    int captureTargetMs = 40;               // latency budget of each capture (drift compensator target)
    int monitorLatencyMs = 40;
};

struct EffectSnapshot {
    std::string type, name;
    bool bypass = false, missing = false;
    std::vector<hb::audio::EffectParamInfo> info;
    std::vector<float> values;  // parallel to info
    float gainReductionDb = 0;
};

struct StripSnapshot {
    int index = 0;
    std::string name;
    InputKind inputKind = InputKind::None;
    std::string inputId, inputName;
    std::string inputState;  // "online" "connecting" "nosignal" "error" "off"
    std::string inputMessage;
    float trimDb = 0, faderDb = 0;
    bool mute = false, solo = false;
    float pan = 0;
    bool phase = false, mono = false;
    float delayMs = 0;
    Quality quality = Quality::Medium;
    bool toProgram = true, toMonitor = false;
    std::vector<EffectSnapshot> effects;
    // levels (dBFS / LUFS, -inf = silence), measured after the effect chain and before the fader
    float peakDb[2] = {}, holdDb[2] = {}, rmsDb[2] = {};
    float lufsShort = 0;
    float gainReductionDb = 0;  // sum over the chain
    std::uint64_t underruns = 0, overruns = 0;
};

struct MasterSnapshot {
    float faderDb = 0;
    bool limiter = true;
    float ceilingDb = -1;
    hb::audio::ProgramLevels levels{};
};

struct MonitorSnapshot {
    std::string deviceId;
    float level = 0.75f;  // fader position 0..1 (0.75 = 0 dB, same law as the strips' faders)
    bool mute = false;
    std::string state;    // "online" "connecting" "nosignal" "error" "off"
    std::string message;
    float peakDb[2] = {};
};

struct EngineStats {
    std::uint64_t blocks = 0;
    std::uint64_t xruns = 0;               // engine thread woke up late (deadline missed)
    std::uint64_t underruns = 0;           // capture/monitor rings that ran dry
    std::uint64_t overruns = 0;            // capture/monitor rings that overflowed
    std::uint64_t consumerErrors = 0;
    double cpuPercent = 0;                 // smoothed block processing time / 10 ms
    double cpuPeakPercent = 0;
};

struct EngineSnapshot {
    unsigned version = 0;
    std::vector<StripSnapshot> strips;
    MasterSnapshot master;
    MonitorSnapshot monitor;
    std::vector<DeviceInfo> devices;
    std::vector<hb::audio::EffectRegistry::Info> effects;
    EngineStats stats;
};

class AudioEngine {
public:
    using ConsumerId = std::uint64_t;
    using ProgramConsumer = std::function<void(const float* stereo48k, std::size_t frames)>;

    explicit AudioEngine(EngineOptions options = {});
    ~AudioEngine();
    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    // ---- lifecycle -------------------------------------------------------------------------
    void start();  // engine thread + maintenance thread; idempotent
    void stop();   // joins both; idempotent (the destructor calls it)
    bool running() const;
    // One 10 ms block. Used by the engine thread; tests call it directly (never while running()).
    void processBlock();
    // Control-side upkeep: rebuilds inputs after a device (re)open, ticks streams (when not
    // supervised) and frees retired snapshots. Called every 50 ms by the maintenance thread.
    void runMaintenance();

    // ---- strips -----------------------------------------------------------------------------
    // keys: trimDb faderDb mute solo pan phase mono delayMs toProgram toMonitor
    bool setStripParam(int strip, const std::string& key, double value);
    bool setStripParam(int strip, const std::string& key, const std::string& value);  // key "name"
    bool setStripQuality(int strip, const std::string& quality);  // linear | medium | high
    // kind: "none" | "device" (id = device id from DeviceEnumerator) | "external" (id registered below)
    bool setStripInput(int strip, const std::string& kind, const std::string& id = "");
    bool registerExternalSource(const std::string& id, std::shared_ptr<hb::audio::AudioSource> source);
    void unregisterExternalSource(const std::string& id);

    // ---- effects (chain executes in order, max 8) ------------------------------------------------
    bool addEffect(int strip, const std::string& typeId, int pos = -1);  // pos<0 or too big: append
    bool removeEffect(int strip, int idx);
    bool moveEffect(int strip, int from, int to);
    bool setEffectBypass(int strip, int idx, bool bypass);
    bool setEffectParam(int strip, int idx, const std::string& paramId, double value);

    // ---- master / monitor ---------------------------------------------------------------------
    bool setMasterParam(const std::string& key, double value);  // faderDb | limiter (0/1) | ceilingDb
    bool setMonitor(const std::string& key, double value);      // level (0..1) | mute (0/1)
    bool setMonitor(const std::string& key, const std::string& value);  // deviceId ("" = off)
    bool refreshDevices();
    void resetMeters();

    // ---- Program output ------------------------------------------------------------------------
    // Called on the engine thread with 480-frame blocks (interleaved stereo, 48 kHz). The removal
    // is synchronous: when it returns the callback is not running and never runs again (unless
    // called from inside the callback itself).
    ConsumerId addProgramConsumer(ProgramConsumer consumer);
    void removeProgramConsumer(ConsumerId id);

    // ---- FEATURE: multitrack ----
    // Per-strip tap for ISO audio recording. Called on the engine thread with 480-frame blocks (interleaved
    // stereo, 48 kHz). postFader=false: the strip after trim/phase/mono/delay/effects, BEFORE fader/mute/solo/pan.
    // postFader=true: the same signal times the strip's fader and mute/solo gate (smoothed per block), still before
    // pan, master fader and limiter. Removal is synchronous like removeProgramConsumer. Returns 0 on bad arguments.
    using StripConsumer = std::function<void(const float* stereo48k, std::size_t frames)>;
    ConsumerId addStripConsumer(int strip, bool postFader, StripConsumer consumer);
    void removeStripConsumer(ConsumerId id);
    // ---- /FEATURE ----

    // ---- state ---------------------------------------------------------------------------------
    unsigned version() const;  // bumps on every configuration change (not on levels)
    EngineSnapshot snapshot() const;
    std::string snapshotJson(bool full) const;  // see docs/AUDIO-ENGINE.md for the exact schema
    EngineStats stats() const;
    std::string lastError() const;
    int stripCount() const;

    // Non-owning access to the live effect instance at (strip, idx), or nullptr if out of range,
    // the slot is empty/missing, or the index moved from under the caller. For capabilities that
    // don't fit the portable Effect interface — e.g. a VST3 effect's own native GUI editor window
    // (src/vst3/), which only the Windows application layer knows how to drive — the caller
    // dynamic_cast's the result to the concrete type it expects. The returned shared_ptr keeps the
    // instance alive even if removeEffect()/the chain changes concurrently; it is still a LIVE
    // real-time object (never call into it from more than one thread at a time beyond what the
    // concrete type itself documents as safe).
    std::shared_ptr<hb::audio::Effect> effectInstance(int strip, int idx) const;

    // ---- persistence ---------------------------------------------------------------------------
    // JSON, schema 1, written atomically (temp file + rename). loadSettings never throws and never
    // crashes on corrupt/hostile files: unreadable or invalid files leave the current state as it
    // is (a fresh engine therefore stays at its defaults) and return false; valid files are
    // sanitised field by field. Unknown effects are kept as "missing" entries.
    bool saveSettings(const std::string& path) const;
    bool loadSettings(const std::string& path);
    std::string settingsJson() const;
    bool applySettingsJson(const std::string& text);
    // Non-fatal remarks of the last load (e.g. newer schema).
    std::vector<std::string> settingsWarnings() const;

    struct Impl;

private:
    std::unique_ptr<Impl> p_;
};

}  // namespace hb::audio_io
