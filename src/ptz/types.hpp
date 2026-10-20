#pragma once
// Common types of the PTZ module. No PTZ function throws through the interface:
// every outcome is a typed PtzResult.
#include <chrono>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace hb::ptz {

using TimePoint = std::chrono::steady_clock::time_point;
using Millis = std::chrono::milliseconds;

enum class PtzStatus { Ok, NotSupported, Timeout, Error };

const char* statusName(PtzStatus s);

struct PtzResult {
    PtzStatus status = PtzStatus::Ok;
    std::string detail;

    bool ok() const { return status == PtzStatus::Ok; }
    static PtzResult success() { return {}; }
    static PtzResult notSupported(std::string d = {}) { return {PtzStatus::NotSupported, std::move(d)}; }
    static PtzResult timeout(std::string d = {}) { return {PtzStatus::Timeout, std::move(d)}; }
    static PtzResult error(std::string d = {}) { return {PtzStatus::Error, std::move(d)}; }
};

template <class T>
struct PtzReply {
    PtzResult result;
    T value{};
    bool ok() const { return result.ok(); }
};

// Normalized motion vector: each axis in [-1, 1].
// +pan = direita, +tilt = cima, +zoom = tele.
struct MoveVector {
    double pan = 0, tilt = 0, zoom = 0;
    bool zero() const { return pan == 0 && tilt == 0 && zoom == 0; }
    bool operator==(const MoveVector&) const = default;
};

struct PresetInfo {
    int id = 0;
    std::string name;
};

enum class PositionUnits { ViscaRaw, Normalized };

struct PtzPosition {
    double pan = 0, tilt = 0, zoom = 0;
    PositionUnits units = PositionUnits::ViscaRaw;
};

struct CameraStatus {
    bool connected = false;
    std::string provider;
    std::optional<PtzPosition> position;
    bool moving = false;  // current movement intent (not a camera reading)
};

// What the provider can do. The UI disables whatever is false.
struct PTZCapabilities {
    bool panTilt = false;
    bool zoom = false;
    bool variableSpeed = false;
    bool presetRecall = false;
    bool presetSave = false;
    bool presetDelete = false;
    bool presetList = false;
    bool focusAuto = false;
    bool focusManual = false;
    bool focusDrive = false;  // near/far
    bool iris = false;
    bool statusInquiry = false;
    bool absoluteMove = false;  // needed for software-stored presets
    int maxPresetId = 0;  // highest accepted preset id; 0 = no presets

    // true if *this covers every feature flagged in `required`.
    bool covers(const PTZCapabilities& required) const;
    // how many of the features flagged in `required` *this covers.
    int coverCount(const PTZCapabilities& required) const;
};

}  // namespace hb::ptz
