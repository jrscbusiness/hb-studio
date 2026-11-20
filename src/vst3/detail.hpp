#pragma once
// Internal implementation details shared by src/vst3/*.cpp. NOT part of the frozen contract
// (src/vst3/vst3.hpp) — free to change as the implementation evolves.
//
// Wraps the bits of the Steinberg VST3 SDK's own hosting helpers (public.sdk/source/vst/hosting/)
// that scanning and effect creation both need: loading a module's factory, finding one class by
// its (lowercase hex) id, and the one process-wide IHostApplication context every component and
// controller we initialize() is handed (this is the SDK's own expected usage: one host context
// per process, not one per plugin instance).
#include <string>

#include "pluginterfaces/base/funknown.h"
#include "public.sdk/source/vst/hosting/module.h"

namespace hb::vst3::detail {

// Lowercase 32-hex-char form of a class UID, matching Vst3PluginInfo::classId / typeIdFor()'s
// format. (VST3::UID::toString() gives uppercase hex.)
std::string lowerHex(const VST3::UID& uid);

// Parses a 32-hex-char id (as produced by lowerHex) back into a UID. Returns false if `hex` isn't
// exactly 32 hex characters.
bool parseLowerHex(const std::string& hex, VST3::UID& out);

// The process-wide IHostApplication context passed to IPluginBase::initialize() for every
// component/controller this module creates. Lazily constructed on first use.
Steinberg::FUnknown* hostContext();

// Finds the class in `factory` whose category is kVstAudioEffectClass and whose id's lowerHex()
// equals `classIdHex`. Returns false (leaving `out` untouched) if there's no such class.
bool findAudioEffectClass(const VST3::Hosting::PluginFactory& factory, const std::string& classIdHex,
                           VST3::Hosting::ClassInfo& out);

}  // namespace hb::vst3::detail
