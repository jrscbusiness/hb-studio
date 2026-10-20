#include "ptz/camera_config.hpp"

#include <algorithm>

namespace hb::ptz {

namespace {
struct Name {
    PtzProtocol p;
    const char* s;
};
constexpr Name kProtocols[] = {{PtzProtocol::OnvifPtz, "onvif"},
                               {PtzProtocol::ViscaIp, "visca-ip"},
                               {PtzProtocol::ViscaSerial, "visca-serial"},
                               {PtzProtocol::PelcoD, "pelco-d"},
                               {PtzProtocol::PelcoP, "pelco-p"}};
}  // namespace

const char* protocolName(PtzProtocol p) {
    for (auto& n : kProtocols)
        if (n.p == p) return n.s;
    return "visca-ip";
}
bool parseProtocol(const std::string& s, PtzProtocol& out) {
    for (auto& n : kProtocols)
        if (s == n.s) {
            out = n.p;
            return true;
        }
    return false;
}
const char* storageName(PresetStorage s) { return s == PresetStorage::Camera ? "camera" : "software"; }
bool parseStorage(const std::string& s, PresetStorage& out) {
    if (s == "camera") out = PresetStorage::Camera;
    else if (s == "software") out = PresetStorage::Software;
    else return false;
    return true;
}

int CameraPtzConfig::defaultPort(PtzProtocol p) {
    switch (p) {
        case PtzProtocol::ViscaIp: return 52381;  // Sony VISCA over IP
        case PtzProtocol::OnvifPtz: return 80;
        default: return 0;  // serial / none
    }
}

void CameraPtzConfig::normalize() {
    presetCount = std::clamp(presetCount, kMinPresetCount, kMaxPresetCount);
    presetBase = presetBase == 0 ? 0 : 1;
    port = std::clamp(port, 0, 65535);
    switch (protocol) {
        case PtzProtocol::ViscaIp: address = 1; break;
        case PtzProtocol::ViscaSerial: address = std::clamp(address, 1, 7); break;
        case PtzProtocol::PelcoD: address = std::clamp(address, 1, 255); break;
        case PtzProtocol::PelcoP: address = std::clamp(address, 0, 31); break;
        case PtzProtocol::OnvifPtz: break;
    }
}

int CameraPtzConfig::deviceId(int button) const {
    if (button < 0 || button >= presetCount) return -1;
    return presetBase + button;
}
int CameraPtzConfig::buttonIndex(int id) const {
    const int b = id - presetBase;
    return (b < 0 || b >= presetCount) ? -1 : b;
}

}  // namespace hb::ptz
