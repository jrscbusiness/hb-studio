#include "vst3/detail.hpp"

#include <algorithm>
#include <cctype>

#include "pluginterfaces/vst/ivstaudioprocessor.h"  // kVstAudioEffectClass
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/hosting/plugprovider.h"  // PluginContextFactory

namespace hb::vst3::detail {

std::string lowerHex(const VST3::UID& uid) {
    std::string s = uid.toString(false);  // uppercase hex, comFormat=false (plain 32 chars)
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool parseLowerHex(const std::string& hex, VST3::UID& out) {
    if (hex.size() != 32) return false;
    for (char c : hex) {
        const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!ok) return false;
    }
    auto parsed = VST3::UID::fromString(hex, false);
    if (!parsed) return false;
    out = *parsed;
    return true;
}

Steinberg::FUnknown* hostContext() {
    using Steinberg::Vst::HostApplication;
    using Steinberg::Vst::PluginContextFactory;
    static Steinberg::IPtr<HostApplication> app = Steinberg::owned(new HostApplication());
    static bool registered = [] {
        PluginContextFactory::instance().setPluginContext(app.get());
        return true;
    }();
    (void)registered;
    return app.get();
}

bool findAudioEffectClass(const VST3::Hosting::PluginFactory& factory, const std::string& classIdHex,
                           VST3::Hosting::ClassInfo& out) {
    for (const auto& info : factory.classInfos()) {
        if (info.category() != kVstAudioEffectClass) continue;
        if (lowerHex(info.ID()) != classIdHex) continue;
        out = info;
        return true;
    }
    return false;
}

}  // namespace hb::vst3::detail
