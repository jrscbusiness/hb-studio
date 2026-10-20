#pragma once
#include <array>
#include <mutex>
#include <string>

namespace hb::app {

// Per-camera PTZ configuration, so that the preset buttons work with any camera:
// the user picks the control protocol and how presets are numbered/stored.
struct PtzCameraConfig {
    std::string protocol = "onvif";  // onvif | visca-ip | visca-serial | pelco-d | pelco-p
    std::string host;                // IP / hostname (or the serial-to-IP converter)
    int port = 0;                    // 0 = protocol default
    int address = 1;                 // VISCA address (1-7) / Pelco address (1-255)
    int slotCount = 10;              // preset buttons shown: 10..64
    int presetBase = 1;              // camera numbers its presets from 0 or 1
    std::string storage = "camera";  // camera (internal memory) | software (position kept by HB Studio)
    int speed = 50;                  // default movement speed 1..100
    bool invertPan = false;
    bool invertTilt = false;
};

// Default TCP/UDP port of a protocol (0 = none / user supplied).
int defaultPtzPort(const std::string& protocol);

// Clamps/sanitises a config coming from the UI or from disk.
PtzCameraConfig sanitize(PtzCameraConfig c);

// Thread-safe store for 8 cameras x up to 64 named preset slots, persisted as JSON.
class PtzSettings {
public:
    static constexpr int kCameras = 8;
    static constexpr int kMaxSlots = 64;

    PtzCameraConfig config(int cam) const;
    bool setConfig(int cam, const PtzCameraConfig& c);
    void applyToAll(const PtzCameraConfig& c);

    std::string presetName(int cam, int slot) const;
    bool setPresetName(int cam, int slot, std::string name);  // empty clears

    unsigned version() const;  // increments on every change

    std::string toJson() const;                 // whole document
    bool fromJson(const std::string& json);     // false = unreadable (state untouched)
    bool load(const std::string& path);         // missing/corrupt file => defaults, returns false
    bool save(const std::string& path) const;   // atomic (temp file + rename)

    // Message for the UI: {"t":"ptz","cams":[...]}
    std::string uiJson() const;

    // Parses one camera config object ({"protocol":...}); false if not valid JSON.
    static bool parseConfig(const std::string& json, PtzCameraConfig& out);

private:
    mutable std::mutex mutex_;
    std::array<PtzCameraConfig, kCameras> cfg_{};
    std::array<std::array<std::string, kMaxSlots>, kCameras> names_{};
    unsigned version_ = 1;
};

}  // namespace hb::app
