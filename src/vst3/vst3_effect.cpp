#include "vst3/vst3_effect.hpp"

#include <algorithm>
#include <cmath>

using Steinberg::int32;
using Steinberg::kResultOk;
using Steinberg::kResultTrue;
namespace Vst = Steinberg::Vst;

namespace hb::vst3 {
namespace {

// A speaker arrangement with exactly `channels` bits set. Not necessarily a "named" layout
// (mono/stereo have dedicated constants we use when they apply), but a plugin only ever looks at
// the bit count via Vst::SpeakerArr::getChannelCount() to decide how many channels to expose, so
// this is enough to request an arbitrary channel count.
Vst::SpeakerArrangement speakerArrangementFor(int channels) {
    if (channels == 1) return Vst::SpeakerArr::kMono;
    if (channels == 2) return Vst::SpeakerArr::kStereo;
    Vst::SpeakerArrangement arr = 0;
    for (int i = 0; i < channels && i < 64; ++i) arr |= (static_cast<Vst::SpeakerArrangement>(1) << i);
    return arr;
}

}  // namespace

Vst3Effect::Vst3Effect(std::shared_ptr<VST3::Hosting::Module> module,
                        Steinberg::IPtr<Vst::PlugProvider> provider, std::string typeId, std::string pluginName)
    : module_(std::move(module)), provider_(std::move(provider)), typeId_(std::move(typeId)),
      pluginName_(std::move(pluginName)) {
    if (provider_) {
        component_ = provider_->getComponentPtr();
        if (component_) {
            Vst::IAudioProcessor* proc = nullptr;
            if (component_->queryInterface(Vst::IAudioProcessor::iid, reinterpret_cast<void**>(&proc)) == kResultTrue)
                processor_ = Steinberg::owned(proc);
        }
    }
}

Vst3Effect::~Vst3Effect() {
#ifdef _WIN32
    closeEditor();  // never leave a native window referencing a component we're about to tear down
#endif
    deactivate();
}

const std::vector<hb::audio::EffectParamInfo>& Vst3Effect::params() const {
    // VST3 effects are driven through their own GUI editor, never HB Studio's generic parameter
    // sliders (see vst3.hpp) — always empty, by contract.
    static const std::vector<hb::audio::EffectParamInfo> kEmpty;
    return kEmpty;
}

void Vst3Effect::setParam(const std::string&, float) {}
float Vst3Effect::getParam(const std::string&) const { return 0.f; }

bool Vst3Effect::activate(int sampleRate, int channels) {
    channels_ = std::clamp(channels, 1, 64);
    hasInputBus_ = false;
    lastError_.clear();
    if (!component_ || !processor_) {
        lastError_ = "componente VST3 inválido (sem IComponent/IAudioProcessor)";
        return false;
    }
    try {
        const int32 nIn = component_->getBusCount(Vst::kAudio, Vst::kInput);
        const int32 nOut = component_->getBusCount(Vst::kAudio, Vst::kOutput);
        if (nOut < 1) {
            lastError_ = "o plugin não expõe nenhum bus de áudio de saída";
            return false;
        }
        hasInputBus_ = nIn >= 1;

        Vst::SpeakerArrangement want = speakerArrangementFor(channels_);
        if (hasInputBus_)
            processor_->setBusArrangements(&want, 1, &want, 1);
        else
            processor_->setBusArrangements(nullptr, 0, &want, 1);
        // setBusArrangements() may return kResultFalse while still adapting its busses (see
        // ivstaudioprocessor.h) — we only care about what getBusInfo() reports afterward, below.

        if (hasInputBus_ && component_->activateBus(Vst::kAudio, Vst::kInput, 0, true) != kResultOk) {
            lastError_ = "falha ao ativar o bus de entrada de áudio";
            return false;
        }
        if (component_->activateBus(Vst::kAudio, Vst::kOutput, 0, true) != kResultOk) {
            lastError_ = "falha ao ativar o bus de saída de áudio";
            if (hasInputBus_) component_->activateBus(Vst::kAudio, Vst::kInput, 0, false);
            return false;
        }

        Vst::BusInfo outInfo{};
        if (component_->getBusInfo(Vst::kAudio, Vst::kOutput, 0, outInfo) == kResultOk &&
            outInfo.channelCount != channels_) {
            lastError_ = "o plugin negociou " + std::to_string(outInfo.channelCount) +
                         " canal(is) de saída, mas " + std::to_string(channels_) + " foram pedidos";
            component_->activateBus(Vst::kAudio, Vst::kOutput, 0, false);
            if (hasInputBus_) component_->activateBus(Vst::kAudio, Vst::kInput, 0, false);
            return false;
        }

        Vst::ProcessSetup setup{};
        setup.processMode = Vst::kRealtime;
        setup.symbolicSampleSize = Vst::kSample32;
        setup.maxSamplesPerBlock = kMaxBlockFrames;
        setup.sampleRate = sampleRate > 0 ? sampleRate : 1;
        if (processor_->setupProcessing(setup) != kResultOk) {
            lastError_ = "IAudioProcessor::setupProcessing falhou";
            component_->activateBus(Vst::kAudio, Vst::kOutput, 0, false);
            if (hasInputBus_) component_->activateBus(Vst::kAudio, Vst::kInput, 0, false);
            return false;
        }
        if (component_->setActive(true) != kResultOk) {
            lastError_ = "IComponent::setActive(true) falhou";
            return false;
        }
        // kNotImplemented is a legitimate answer here (setProcessing() is informational — see
        // ivstaudioprocessor.h — not every plugin needs the notification), only a hard failure
        // code fails activation.
        const Steinberg::tresult processingResult = processor_->setProcessing(true);
        if (processingResult != kResultOk && processingResult != Steinberg::kNotImplemented) {
            lastError_ = "IAudioProcessor::setProcessing(true) falhou";
            component_->setActive(false);
            return false;
        }
    } catch (const std::exception& e) {
        lastError_ = std::string("o plugin lançou uma exceção ao preparar: ") + e.what();
        return false;
    } catch (...) {
        lastError_ = "o plugin lançou uma exceção desconhecida ao preparar";
        return false;
    }

    inputPlanar_.assign(static_cast<std::size_t>(channels_), std::vector<float>(kMaxBlockFrames, 0.f));
    outputPlanar_.assign(static_cast<std::size_t>(channels_), std::vector<float>(kMaxBlockFrames, 0.f));
    inPtrs_.assign(static_cast<std::size_t>(channels_), nullptr);
    outPtrs_.assign(static_cast<std::size_t>(channels_), nullptr);
    for (int c = 0; c < channels_; ++c) {
        inPtrs_[static_cast<std::size_t>(c)] = inputPlanar_[static_cast<std::size_t>(c)].data();
        outPtrs_[static_cast<std::size_t>(c)] = outputPlanar_[static_cast<std::size_t>(c)].data();
    }

    inBus_ = Vst::AudioBusBuffers{};
    inBus_.numChannels = channels_;
    inBus_.channelBuffers32 = inPtrs_.data();
    outBus_ = Vst::AudioBusBuffers{};
    outBus_.numChannels = channels_;
    outBus_.channelBuffers32 = outPtrs_.data();

    processContext_ = Vst::ProcessContext{};
    inputParamChanges_.setMaxParameters(0);

    processData_ = Vst::ProcessData{};
    processData_.processMode = Vst::kRealtime;
    processData_.symbolicSampleSize = Vst::kSample32;
    processData_.numInputs = hasInputBus_ ? 1 : 0;
    processData_.numOutputs = 1;
    processData_.inputs = hasInputBus_ ? &inBus_ : nullptr;
    processData_.outputs = &outBus_;
    processData_.inputParameterChanges = &inputParamChanges_;
    processData_.processContext = &processContext_;

    consecutiveBad_ = 0;
    selfBypassed_.store(false, std::memory_order_relaxed);
    return true;
}

void Vst3Effect::deactivate() noexcept {
    try {
        if (processor_) processor_->setProcessing(false);
        if (component_) {
            component_->activateBus(Vst::kAudio, Vst::kOutput, 0, false);
            if (hasInputBus_) component_->activateBus(Vst::kAudio, Vst::kInput, 0, false);
            component_->setActive(false);
        }
    } catch (...) {
        errorCount_.fetch_add(1, std::memory_order_relaxed);
    }
}

void Vst3Effect::prepare(int sampleRate, int channels) {
    deactivate();
    activationOk_.store(activate(sampleRate, channels), std::memory_order_relaxed);
}

void Vst3Effect::process(float* interleaved, std::size_t frames) {
    if (interleaved == nullptr || frames == 0 || channels_ <= 0) return;
    const std::size_t ch = static_cast<std::size_t>(channels_);
    const std::size_t n = frames * ch;
    for (std::size_t i = 0; i < n; ++i)
        if (!std::isfinite(interleaved[i])) interleaved[i] = 0.f;

    if (!activationOk_.load(std::memory_order_relaxed) || selfBypassed_.load(std::memory_order_relaxed) ||
        !processor_) {
        return;  // pass-through, like the other native-plugin adapters in this project
    }

    std::size_t offset = 0;
    bool bad = false;
    while (offset < frames) {
        const std::size_t chunk = std::min(frames - offset, static_cast<std::size_t>(kMaxBlockFrames));
        if (hasInputBus_) {
            for (std::size_t c = 0; c < ch; ++c) {
                float* dst = inputPlanar_[c].data();
                for (std::size_t i = 0; i < chunk; ++i) dst[i] = interleaved[(offset + i) * ch + c];
            }
        }
        processData_.numSamples = static_cast<int32>(chunk);

        bool ok;
        try {
            ok = processor_->process(processData_) == kResultOk;
        } catch (...) {
            ok = false;
        }
        if (ok) {
            for (std::size_t c = 0; c < ch; ++c) {
                const float* src = outputPlanar_[c].data();
                for (std::size_t i = 0; i < chunk; ++i) {
                    float v = src[i];
                    if (!std::isfinite(v)) {
                        v = 0.f;
                        ok = false;
                    }
                    interleaved[(offset + i) * ch + c] = v;
                }
            }
        }
        if (!ok) {
            bad = true;
            break;
        }
        offset += chunk;
    }

    if (bad) {
        for (std::size_t i = offset * ch; i < n; ++i) interleaved[i] = 0.f;
        errorCount_.fetch_add(1, std::memory_order_relaxed);
        if (++consecutiveBad_ >= kMaxConsecutiveBad) selfBypassed_.store(true, std::memory_order_relaxed);
    } else {
        consecutiveBad_ = 0;
    }
}

void Vst3Effect::reset() {
    if (processor_ && activationOk_.load(std::memory_order_relaxed)) {
        try {
            processor_->setProcessing(false);
            processor_->setProcessing(true);
        } catch (...) {
            errorCount_.fetch_add(1, std::memory_order_relaxed);
        }
    }
    for (auto& c : inputPlanar_) std::fill(c.begin(), c.end(), 0.f);
    for (auto& c : outputPlanar_) std::fill(c.begin(), c.end(), 0.f);
    consecutiveBad_ = 0;
}

#ifdef _WIN32
Steinberg::IPtr<Vst::IEditController> Vst3Effect::controllerForEditor() const {
    return provider_ ? provider_->getControllerPtr() : nullptr;
}
#endif

}  // namespace hb::vst3
