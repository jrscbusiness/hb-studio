#include "onvif/messages.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace hb::onvif::messages {
namespace {

// Locale-independent double formatting ("0.5", never "0,5"), clamped to [-1, 1].
std::string num(double v, double lo = -1.0, double hi = 1.0) {
    if (!std::isfinite(v)) v = 0;
    v = std::clamp(v, lo, hi);
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.4f", v);
    std::string s = buf;
    for (auto& c : s)
        if (c == ',') c = '.';
    return s;
}

std::string tok(const char* tag, const std::string& v) {
    return std::string("<") + tag + ">" + xmlEscape(v) + "</" + tag + ">";
}

Message make(const char* nsUri, const char* op, const std::string& inner, bool auth = true) {
    Message m;
    m.action = std::string(nsUri) + "/" + op;
    m.bodyXml = std::string("<") + op + " xmlns=\"" + nsUri + "\"" + (inner.empty() ? "/>" : ">" + inner + "</" + op + ">");
    m.requiresAuth = auth;
    return m;
}

}  // namespace

Message getDeviceInformation() { return make(ns::kDevice, "GetDeviceInformation", ""); }
Message getSystemDateAndTime() { return make(ns::kDevice, "GetSystemDateAndTime", "", false); }
Message getCapabilities() { return make(ns::kDevice, "GetCapabilities", "<Category>All</Category>"); }
Message getServices(bool includeCapability) {
    return make(ns::kDevice, "GetServices",
                std::string("<IncludeCapability>") + (includeCapability ? "true" : "false") + "</IncludeCapability>");
}

Message getProfiles() { return make(ns::kMedia, "GetProfiles", ""); }

Message getStreamUri(const std::string& profileToken) {
    std::string inner = std::string("<StreamSetup><Stream xmlns=\"") + ns::kSchema + "\">RTP-Unicast</Stream><Transport xmlns=\"" +
                        ns::kSchema + "\"><Protocol>" + "RTSP" + "</Protocol></Transport></StreamSetup>" +
                        tok("ProfileToken", profileToken);
    return make(ns::kMedia, "GetStreamUri", inner);
}

Message ptzContinuousMove(const std::string& profileToken, double pan, double tilt, double zoom, double timeoutSeconds) {
    std::string inner = tok("ProfileToken", profileToken);
    inner += std::string("<Velocity><PanTilt xmlns=\"") + ns::kSchema + "\" x=\"" + num(pan) + "\" y=\"" + num(tilt) +
             "\"/><Zoom xmlns=\"" + ns::kSchema + "\" x=\"" + num(zoom) + "\"/></Velocity>";
    if (timeoutSeconds > 0) {
        char buf[40];
        std::snprintf(buf, sizeof buf, "PT%.1fS", std::min(timeoutSeconds, 3600.0));
        std::string t = buf;
        for (auto& c : t)
            if (c == ',') c = '.';
        inner += "<Timeout>" + t + "</Timeout>";
    }
    return make(ns::kPtz, "ContinuousMove", inner);
}

Message ptzStop(const std::string& profileToken, bool panTilt, bool zoom) {
    return make(ns::kPtz, "Stop",
                tok("ProfileToken", profileToken) + "<PanTilt>" + (panTilt ? "true" : "false") + "</PanTilt><Zoom>" +
                    (zoom ? "true" : "false") + "</Zoom>");
}

Message ptzGotoPreset(const std::string& profileToken, const std::string& presetToken) {
    return make(ns::kPtz, "GotoPreset", tok("ProfileToken", profileToken) + tok("PresetToken", presetToken));
}

Message ptzSetPreset(const std::string& profileToken, const std::string& presetName, const std::string& presetToken) {
    std::string inner = tok("ProfileToken", profileToken);
    if (!presetName.empty()) inner += tok("PresetName", presetName);
    if (!presetToken.empty()) inner += tok("PresetToken", presetToken);
    return make(ns::kPtz, "SetPreset", inner);
}

Message ptzRemovePreset(const std::string& profileToken, const std::string& presetToken) {
    return make(ns::kPtz, "RemovePreset", tok("ProfileToken", profileToken) + tok("PresetToken", presetToken));
}

Message ptzGetPresets(const std::string& profileToken) { return make(ns::kPtz, "GetPresets", tok("ProfileToken", profileToken)); }
Message ptzGetStatus(const std::string& profileToken) { return make(ns::kPtz, "GetStatus", tok("ProfileToken", profileToken)); }

Message imagingGetSettings(const std::string& videoSourceToken) {
    return make(ns::kImaging, "GetImagingSettings", tok("VideoSourceToken", videoSourceToken));
}

Message imagingMoveFocus(const std::string& videoSourceToken, double speed) {
    return make(ns::kImaging, "Move",
                tok("VideoSourceToken", videoSourceToken) + "<Focus><Continuous><Speed xmlns=\"" + ns::kSchema + "\">" +
                    num(speed) + "</Speed></Continuous></Focus>");
}

Message imagingStop(const std::string& videoSourceToken) {
    return make(ns::kImaging, "Stop", tok("VideoSourceToken", videoSourceToken));
}

}  // namespace hb::onvif::messages
