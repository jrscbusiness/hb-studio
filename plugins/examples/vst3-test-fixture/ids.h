#pragma once
// Class IDs for the hb_vst3_test_fixture plugin — see README.md in this folder: this is a
// project-authored test fixture, not a real/commercial plugin. Generated once, arbitrarily; only
// required to be two distinct, stable 16-byte ids.
#include "pluginterfaces/base/funknown.h"

namespace hb_fixture {

static const Steinberg::FUID kGainProcessorUID(0x7b3f9a10, 0xc4a1486a, 0x9e2d6f51, 0x0c8b2a77);
static const Steinberg::FUID kGainControllerUID(0x7b3f9a11, 0xc4a1486a, 0x9e2d6f51, 0x0c8b2a78);

}  // namespace hb_fixture
