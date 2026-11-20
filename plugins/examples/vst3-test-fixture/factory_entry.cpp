// VST3 module entry point + class factory for hb_vst3_test_fixture.
// See README.md in this folder: a project-authored test fixture, not a real plugin.
#include "gain_processor.h"
#include "ids.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"  // kVstAudioEffectClass, kDistributable
#include "pluginterfaces/vst/ivsteditcontroller.h"  // kVstComponentControllerClass
#include "public.sdk/source/main/pluginfactory.h"
#include "public.sdk/source/vst/vsteditcontroller.h"

#define kFixtureVendor "HB Studio (test fixture)"
#define kFixtureURL "https://example.invalid/hb-studio"
#define kFixtureEmail "mailto:noreply@example.invalid"
#define kFixtureVersion "1.0.0"

using namespace Steinberg;

namespace {
// No custom controller behaviour needed (no parameters, no UI): the SDK's own EditController base
// already does exactly what this fixture needs — createView() returns nullptr, so hasEditor() is
// correctly false.
FUnknown* createFixtureController(void*) {
    return static_cast<Vst::IEditController*>(new Vst::EditController());
}
}  // namespace

BEGIN_FACTORY_DEF(kFixtureVendor, kFixtureURL, kFixtureEmail)

DEF_CLASS2(INLINE_UID_FROM_FUID(hb_fixture::kGainProcessorUID), PClassInfo::kManyInstances, kVstAudioEffectClass,
           "HB Test Gain (fixture)", Vst::kDistributable, "Fx", kFixtureVersion, kVstVersionString,
           hb_fixture::GainProcessor::createInstance)

DEF_CLASS2(INLINE_UID_FROM_FUID(hb_fixture::kGainControllerUID), PClassInfo::kManyInstances,
           kVstComponentControllerClass, "HB Test Gain (fixture) Controller", 0, "", kFixtureVersion,
           kVstVersionString, createFixtureController)

END_FACTORY

// Required by public.sdk/source/main/{linux,win32}main.cpp; nothing to set up/tear down.
bool InitModule() { return true; }
bool DeinitModule() { return true; }
