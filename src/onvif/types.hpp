#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace hb::onvif {

struct Credentials {
    std::string username;
    std::string password;  // never logged
};

struct DeviceInformation {
    std::string manufacturer, model, firmwareVersion, serialNumber, hardwareId;
};

struct SystemDateTime {
    std::int64_t utcSeconds = 0;  // device UTC clock, seconds since epoch
    std::string dateTimeType;     // "NTP" / "Manual" (informational)
};

// XAddrs of the services we use. Empty string = service absent.
struct ServiceEndpoints {
    std::string device, media, ptz, imaging, events;
};

struct VideoEncoderConfig {
    std::string token;
    std::string encoding;  // normalised: "H264", "H265", "JPEG", "MPEG4" or raw text
    int width = 0, height = 0;
    double frameRate = 0;
    int bitrateKbps = 0;
    int gopLength = 0;
};

struct MediaProfile {
    std::string token, name;
    bool fixed = false;
    std::optional<VideoEncoderConfig> video;
    std::string videoSourceToken;
    std::string ptzConfigToken;  // non-empty => profile has PTZ configuration
    bool hasPtz() const { return !ptzConfigToken.empty(); }
};

struct StreamUri {
    std::string uri;
    bool invalidAfterConnect = false;
    bool invalidAfterReboot = false;
    std::string timeout;  // xs:duration, informational
};

struct PtzPreset {
    std::string token, name;
};

struct PtzStatus {
    std::optional<double> pan, tilt, zoom;       // normalised positions when reported
    std::string panTiltMove, zoomMove;           // IDLE / MOVING / UNKNOWN, "" if absent
};

struct ImagingSettings {
    std::optional<double> brightness, contrast, colorSaturation, sharpness;
    std::string focusMode;      // AUTO / MANUAL, "" if absent
    std::string irCutFilter;    // ON / OFF / AUTO
};

}  // namespace hb::onvif
