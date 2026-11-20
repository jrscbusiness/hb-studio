#pragma once
// VST3 effect plugin hosting: audio processing plus the plugin's own native GUI editor window.
// FROZEN INTERFACE: src/vst3/ (implementation) and the application (wiring into EffectRegistry /
// the ÁUDIO tab) are written against this header.
//
// Scope, decided by the project owner: VST3 only (VST2 is deprecated by Steinberg and no longer
// licensable for new hosts — a legal grey area this project avoids entirely), and the plugin's own
// GUI editor window (not a generic parameter-slider list) — the operator opens it in its own
// window, the same experience as hosting a VST3 plugin in any other DAW.
//
// Licensing: since SDK version 3.8 (tag v3.8.1_build_84, verified directly against the SDK's own
// LICENSE.txt), the whole Steinberg VST3 SDK is MIT-licensed — Steinberg's README says the older
// GPLv3 and "Steinberg proprietary license" choices are "no longer available". No registration
// with Steinberg is required and HB Studio's own license is unaffected (MIT is compatible with
// anything). The one thing MIT does NOT cover is the "VST" name/logo, which is a separate
// trademark: per the SDK's README, using the name/logo is optional, but if used it must follow
// Steinberg's trademark guidelines (attribution text, no implied endorsement) — see LICENSES.md.
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "audio/effect.hpp"

namespace hb::vst3 {

struct Vst3PluginInfo {
    std::string path;      // .vst3 module path (single file or bundle) on disk
    std::string classId;   // 32 lowercase hex chars (the class's FUID) — stable identifier
    std::string name;      // e.g. "ValhallaRoom"
    std::string vendor;
    std::string category;  // Steinberg's own category string, e.g. "Fx|Reverb"
};

// Scans the standard VST3 install folders (on Windows: %COMMONPROGRAMFILES%\VST3 and
// %LOCALAPPDATA%\Programs\Common\VST3) plus any in extraFolders, for every class of category
// kVstAudioEffectClass in every module found. Loads each module just long enough to read its
// factory's class list, then unloads it again — scanning never leaves anything resident. Never
// throws; a module that fails to load, isn't really a VST3 module, or doesn't expose usable
// classes is silently skipped, never fatal to the scan.
std::vector<Vst3PluginInfo> scanPlugins(const std::vector<std::string>& extraFolders = {});

// The typeId HB Studio registers this plugin's effect under in EffectRegistry, stable across
// rescans as long as the same module+classId is found (used as the "type" when saving/restoring a
// strip's effect chain). Format: "vst3:<classId-32-hex>".
std::string typeIdFor(const Vst3PluginInfo& info);

// Loads `info` and returns an Effect instance ready to hand to EffectRegistry::add()'s factory, or
// nullptr (with `error`, pt-BR) if the module/class can't be instantiated or activated for
// `sampleRate`/`channels`-shaped processing (the instance is still prepared normally via
// Effect::prepare() afterward; this call just proves it CAN be created at all so a dead plugin is
// never silently registered). params() on the returned Effect is always empty: VST3 effects are
// driven through their own GUI editor (below), not generic sliders — the only control HB Studio's
// own UI offers directly is the per-effect bypass toggle every effect already has (this disables
// IAudioProcessor::process() the same way any other effect's bypass does; it does not touch the
// plugin itself). process()/prepare()/reset() must never throw and must behave within the same
// real-time constraints documented on hb::audio::Effect — like every other native plugin HB
// Studio hosts, a VST3 plugin that misbehaves (hangs, corrupts memory) runs in-process and cannot
// be fully contained by this interface; see docs/PLUGINS.md's security model, which applies here
// too (VST3 plugins should be subject to the same "install only from authors you trust" posture).
std::unique_ptr<hb::audio::Effect> createEffect(const Vst3PluginInfo& info, std::string& error);

// Windows-only native GUI editor window for one VST3 effect instance. Not every Effect the
// registry creates is one of these — the application gets an Effect* from
// AudioEngine::effectInstance() and dynamic_cast's it to IVst3Editor to find out (nullptr: not a
// VST3 effect, or hasEditor() false: a VST3 effect whose plugin didn't implement
// IEditController/IPlugView). One editor window per effect instance; opening it twice just shows
// the existing window. UI/control thread only — never call from the real-time audio thread.
// Closing the window (by the operator or via closeEditor()) never affects audio processing, which
// keeps running regardless of whether the editor is open; conversely, destroying the Effect
// instance (removeEffect()) closes any open editor window for it first.
class IVst3Editor {
public:
    virtual ~IVst3Editor() = default;
    virtual bool hasEditor() const = 0;  // false: this plugin didn't implement an editor at all
    virtual bool editorOpen() const = 0;
    // Opens (or brings to front, if already open) the plugin's own editor window. `title` is shown
    // in the window's caption (e.g. "ValhallaRoom — CANAL 1 · efeito 2"). Returns false (with
    // `error`, pt-BR) on failure; the effect keeps processing audio regardless of the outcome.
    virtual bool openEditor(const std::string& title, std::string& error) = 0;
    virtual void closeEditor() = 0;
};

}  // namespace hb::vst3
