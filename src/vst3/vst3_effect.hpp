#pragma once
// Vst3Effect: hosts one instance of a VST3 effect class as an hb::audio::Effect. Portable
// (audio-only processing, usable in tests on Linux); on Windows it ALSO implements IVst3Editor
// (src/vst3/vst3.hpp) so AudioEngine::effectInstance()'s dynamic_cast finds the editor on the
// very same object — the Windows-only parts (the editor's native window, IPlugFrame, HWND
// attach/removed) live in src/vst3/windows/vst3_editor.cpp and are compiled only under WIN32; see
// that file for the (necessarily untested outside Windows CI) editor implementation.
//
// Internal detail of src/vst3/ — not part of the frozen vst3.hpp contract.
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "audio/effect.hpp"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "public.sdk/source/vst/hosting/module.h"
#include "public.sdk/source/vst/hosting/parameterchanges.h"
#include "public.sdk/source/vst/hosting/plugprovider.h"
#include "vst3/vst3.hpp"

namespace hb::vst3 {

class Vst3Effect final : public hb::audio::Effect
#ifdef _WIN32
    ,
                          public IVst3Editor
#endif
{
public:
    // `module` keeps the .vst3 library loaded for as long as this effect exists; `provider` owns
    // the already-initialized (and, if separate, connected) IComponent/IEditController pair.
    Vst3Effect(std::shared_ptr<VST3::Hosting::Module> module,
               Steinberg::IPtr<Steinberg::Vst::PlugProvider> provider, std::string typeId, std::string pluginName);
    ~Vst3Effect() override;

    Vst3Effect(const Vst3Effect&) = delete;
    Vst3Effect& operator=(const Vst3Effect&) = delete;

    // --- hb::audio::Effect ---------------------------------------------------------------
    const char* typeId() const override { return typeId_.c_str(); }
    const std::vector<hb::audio::EffectParamInfo>& params() const override;
    void setParam(const std::string& id, float value) override;
    float getParam(const std::string& id) const override;
    void prepare(int sampleRate, int channels) override;
    void process(float* interleaved, std::size_t frames) override;
    void reset() override;

    // --- internal, used by vst3.cpp's createEffect() --------------------------------------
    // True once prepare() has actually activated the plugin for processing. createEffect() calls
    // prepare() once as a smoke test (see vst3.hpp's comment on createEffect) and checks this.
    bool activationOk() const noexcept { return activationOk_.load(std::memory_order_relaxed); }
    // Reason the last prepare()/activate() failed, pt-BR. Only meaningful right after a failed
    // prepare(); not updated from process() (which must stay allocation/lock free).
    const std::string& lastError() const noexcept { return lastError_; }
    std::uint64_t errorCount() const noexcept { return errorCount_.load(std::memory_order_relaxed); }
    bool selfBypassed() const noexcept { return selfBypassed_.load(std::memory_order_relaxed); }
    const std::string& pluginName() const noexcept { return pluginName_; }

#ifdef _WIN32
    // --- IVst3Editor -----------------------------------------------------------------------
    bool hasEditor() const override;
    bool editorOpen() const override;
    bool openEditor(const std::string& title, std::string& error) override;
    void closeEditor() override;
#endif

private:
    // Real-time budget: process() never allocates, so planar scratch buffers are sized to this
    // many frames up front (in prepare(), which is NOT real-time) and process() loops in chunks
    // of at most this size for a larger block.
    static constexpr int kMaxBlockFrames = 4096;
    // After this many consecutive bad blocks (plugin threw, returned an error, or produced
    // non-finite output) process() stops calling into the plugin and passes audio through
    // untouched — same crash-containment policy as src/audio/plugin_effect.cpp.
    static constexpr int kMaxConsecutiveBad = 8;

    bool activate(int sampleRate, int channels);  // never throws; returns success, fills lastError_
    void deactivate() noexcept;

#ifdef _WIN32
    Steinberg::IPtr<Steinberg::Vst::IEditController> controllerForEditor() const;
    struct EditorState;
    static void destroyEditorState(EditorState*) noexcept;
#endif

    // Declared in this order so members destruct in reverse: processor_/component_ (which hold
    // extra AddRef'd references) release first, then provider_ (which owns the "real" references
    // and runs IPluginBase::terminate()/disconnect on destruction), then the module unloads.
    std::shared_ptr<VST3::Hosting::Module> module_;
    Steinberg::IPtr<Steinberg::Vst::PlugProvider> provider_;
    Steinberg::IPtr<Steinberg::Vst::IComponent> component_;
    Steinberg::IPtr<Steinberg::Vst::IAudioProcessor> processor_;

    std::string typeId_;
    std::string pluginName_;
    std::string lastError_;  // set only from prepare()/activate() (not real-time)

    int channels_ = 0;
    bool hasInputBus_ = false;
    std::vector<std::vector<float>> inputPlanar_, outputPlanar_;
    std::vector<float*> inPtrs_, outPtrs_;
    Steinberg::Vst::AudioBusBuffers inBus_{}, outBus_{};
    Steinberg::Vst::ProcessData processData_{};
    Steinberg::Vst::ProcessContext processContext_{};
    Steinberg::Vst::ParameterChanges inputParamChanges_;

    std::atomic<bool> activationOk_{false};
    std::atomic<bool> selfBypassed_{false};
    std::atomic<std::uint64_t> errorCount_{0};
    int consecutiveBad_ = 0;  // process() only, single real-time caller

#ifdef _WIN32
    std::unique_ptr<EditorState, decltype(&destroyEditorState)> editor_{nullptr, &destroyEditorState};
#endif
};

}  // namespace hb::vst3
