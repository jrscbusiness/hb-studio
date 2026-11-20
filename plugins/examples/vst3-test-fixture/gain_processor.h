#pragma once
// See README.md in this folder: a project-authored test fixture, not a real plugin.
#include "public.sdk/source/vst/vstaudioeffect.h"

namespace hb_fixture {

// Fixed -6 dB (x0.5) gain, no parameters, no side effects: easy to assert on in tests. Accepts
// whatever channel count the host negotiates via setBusArrangements() (the AudioEffect base
// class's default implementation already adapts both busses to match, which is all this fixture
// needs).
class GainProcessor final : public Steinberg::Vst::AudioEffect {
public:
    GainProcessor();

    Steinberg::tresult PLUGIN_API initialize(Steinberg::FUnknown* context) override;
    Steinberg::tresult PLUGIN_API process(Steinberg::Vst::ProcessData& data) override;

    static Steinberg::FUnknown* createInstance(void*);
};

}  // namespace hb_fixture
