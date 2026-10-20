#pragma once
// Per-camera PTZ configuration (what the settings UI edits).
#include <string>

namespace hb::ptz {

enum class PtzProtocol { OnvifPtz, ViscaIp, ViscaSerial, PelcoD, PelcoP };
enum class PresetStorage { Camera, Software };  // where the preset position lives

const char* protocolName(PtzProtocol p);
bool parseProtocol(const std::string& s, PtzProtocol& out);
const char* storageName(PresetStorage s);
bool parseStorage(const std::string& s, PresetStorage& out);

constexpr int kMinPresetCount = 10;
constexpr int kMaxPresetCount = 64;

struct CameraPtzConfig {
    std::string cameraId;
    PtzProtocol protocol = PtzProtocol::ViscaIp;
    std::string host;  // IP / hostname (network protocols) or serial port name
    int port = 0;      // 0 = protocol default
    int address = 1;   // VISCA 1..7, Pelco-D 1..255, Pelco-P 0..31
    int presetCount = 16;  // number of preset buttons, 10..64
    int presetBase = 1;    // first preset number on the wire: 0 or 1
    PresetStorage storage = PresetStorage::Camera;

    // Clamps every field into its legal range.
    void normalize();

    // UI button index (0..presetCount-1) -> preset number sent to the camera; -1 if out of range.
    int deviceId(int button) const;
    // Inverse; -1 if the preset number is outside the configured range.
    int buttonIndex(int deviceId) const;

    int effectivePort() const { return port > 0 ? port : defaultPort(protocol); }
    static int defaultPort(PtzProtocol p);

    bool operator==(const CameraPtzConfig&) const = default;
};

}  // namespace hb::ptz
