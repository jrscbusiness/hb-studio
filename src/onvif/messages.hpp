#pragma once
#include <optional>
#include <string>

#include "onvif/soap.hpp"

namespace hb::onvif::messages {

// Device management
Message getDeviceInformation();
Message getSystemDateAndTime();  // unauthenticated by design
Message getCapabilities();
Message getServices(bool includeCapability = false);

// Media (ver10)
Message getProfiles();
Message getStreamUri(const std::string& profileToken);  // RTP-Unicast over RTSP

// PTZ. Velocities/positions normalised to [-1, 1] (clamped). timeoutSeconds <= 0 means "no Timeout element".
Message ptzContinuousMove(const std::string& profileToken, double pan, double tilt, double zoom, double timeoutSeconds = 0);
Message ptzStop(const std::string& profileToken, bool panTilt = true, bool zoom = true);
Message ptzGotoPreset(const std::string& profileToken, const std::string& presetToken);
Message ptzSetPreset(const std::string& profileToken, const std::string& presetName, const std::string& presetToken = {});
Message ptzRemovePreset(const std::string& profileToken, const std::string& presetToken);
Message ptzGetPresets(const std::string& profileToken);
Message ptzGetStatus(const std::string& profileToken);

// Imaging
Message imagingGetSettings(const std::string& videoSourceToken);
Message imagingMoveFocus(const std::string& videoSourceToken, double continuousSpeed);  // [-1, 1]
Message imagingStop(const std::string& videoSourceToken);

}  // namespace hb::onvif::messages
