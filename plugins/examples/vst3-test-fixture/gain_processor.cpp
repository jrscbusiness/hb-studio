#include "gain_processor.h"

#include "ids.h"
#include "pluginterfaces/base/fstrdefs.h"
#include "pluginterfaces/base/ustring.h"

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace hb_fixture {

namespace {
constexpr float kGain = 0.5f;  // -6 dB, fixed
}

GainProcessor::GainProcessor() { setControllerClass(kGainControllerUID); }

tresult PLUGIN_API GainProcessor::initialize(FUnknown* context) {
    tresult result = AudioEffect::initialize(context);
    if (result != kResultOk) return result;
    addAudioInput(STR16("Stereo In"), SpeakerArr::kStereo);
    addAudioOutput(STR16("Stereo Out"), SpeakerArr::kStereo);
    return kResultOk;
}

tresult PLUGIN_API GainProcessor::process(ProcessData& data) {
    if (data.numSamples <= 0) return kResultOk;
    if (data.numOutputs < 1 || data.outputs == nullptr) return kResultOk;

    const int32 outChannels = data.outputs[0].numChannels;
    const bool haveInput = data.numInputs >= 1 && data.inputs != nullptr;
    const int32 inChannels = haveInput ? data.inputs[0].numChannels : 0;

    for (int32 c = 0; c < outChannels; ++c) {
        float* out = data.outputs[0].channelBuffers32[c];
        if (out == nullptr) continue;
        if (haveInput && c < inChannels && data.inputs[0].channelBuffers32[c] != nullptr) {
            const float* in = data.inputs[0].channelBuffers32[c];
            for (int32 i = 0; i < data.numSamples; ++i) out[i] = in[i] * kGain;
        } else {
            for (int32 i = 0; i < data.numSamples; ++i) out[i] = 0.f;
        }
    }
    return kResultOk;
}

FUnknown* GainProcessor::createInstance(void*) { return static_cast<IAudioProcessor*>(new GainProcessor()); }

}  // namespace hb_fixture
