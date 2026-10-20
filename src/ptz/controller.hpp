#pragma once
// Generic PTZ control interface + provider base class.
#include <string>
#include <vector>

#include "ptz/types.hpp"

namespace hb::ptz {

// Generic interface. Contract: NO method throws; every result is typed.
// Speeds are normalized 0..1 (magnitude); the direction comes from the method called.
class PTZController {
public:
    virtual ~PTZController() = default;

    virtual const PTZCapabilities& capabilities() const = 0;

    virtual PtzResult connect() = 0;
    virtual PtzResult disconnect() = 0;

    virtual PtzResult panLeft() = 0;
    virtual PtzResult panRight() = 0;
    virtual PtzResult tiltUp() = 0;
    virtual PtzResult tiltDown() = 0;
    virtual PtzResult stop() = 0;
    virtual PtzResult zoomIn() = 0;   // tele
    virtual PtzResult zoomOut() = 0;  // wide
    virtual PtzResult setPanSpeed(double speed01) = 0;
    virtual PtzResult setTiltSpeed(double speed01) = 0;
    virtual PtzResult setZoomSpeed(double speed01) = 0;

    virtual PtzResult gotoPreset(int id) = 0;
    virtual PtzResult savePreset(int id) = 0;
    virtual PtzResult deletePreset(int id) = 0;
    virtual PtzReply<std::vector<PresetInfo>> getPresets() = 0;

    virtual PtzResult focusAuto() = 0;
    virtual PtzResult focusManual() = 0;
    virtual PtzResult focusNear() = 0;
    virtual PtzResult focusFar() = 0;
    virtual PtzResult irisOpen() = 0;
    virtual PtzResult irisClose() = 0;

    virtual PtzReply<CameraStatus> getCameraStatus() = 0;

    // Extra: absolute positioning (used by software-stored presets). Optional.
    virtual PtzResult moveAbsolute(const PtzPosition&) { return PtzResult::notSupported("absolute move"); }
};

// Provider base: implements the motion methods on top of drive(), keeping
// direction/speed state (pan+tilt+zoom are sent together as one vector).
class PTZProvider : public PTZController {
public:
    virtual std::string name() const = 0;

    // Applies the vector (each axis in [-1,1]). Zero vector = stop pan/tilt/zoom.
    virtual PtzResult drive(const MoveVector& v) = 0;
    // Unconditional pan/tilt/zoom stop (resent even if the state was already zero).
    virtual PtzResult stopAll() { return drive({}); }
    // Stops continuous focus (near/far). Default: nothing to stop.
    virtual PtzResult stopLens() { return PtzResult::success(); }

    PtzResult panLeft() override { panDir_ = -1; return push(); }
    PtzResult panRight() override { panDir_ = 1; return push(); }
    PtzResult tiltUp() override { tiltDir_ = 1; return push(); }
    PtzResult tiltDown() override { tiltDir_ = -1; return push(); }
    PtzResult zoomIn() override { zoomDir_ = 1; return push(); }
    PtzResult zoomOut() override { zoomDir_ = -1; return push(); }
    PtzResult stop() override {
        panDir_ = tiltDir_ = zoomDir_ = 0;
        auto a = stopAll();
        auto b = stopLens();
        return a.ok() ? b : a;
    }
    PtzResult setPanSpeed(double s) override { panSpeed_ = clamp01(s); return refresh(); }
    PtzResult setTiltSpeed(double s) override { tiltSpeed_ = clamp01(s); return refresh(); }
    PtzResult setZoomSpeed(double s) override { zoomSpeed_ = clamp01(s); return refresh(); }

    static double clamp01(double v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

protected:
    MoveVector vector() const {
        return {panDir_ * panSpeed_, tiltDir_ * tiltSpeed_, zoomDir_ * zoomSpeed_};
    }

private:
    PtzResult push() { return drive(vector()); }
    // Changing speed only resends if a movement is in progress.
    PtzResult refresh() { return (panDir_ || tiltDir_ || zoomDir_) ? push() : PtzResult::success(); }

    int panDir_ = 0, tiltDir_ = 0, zoomDir_ = 0;
    double panSpeed_ = 0.5, tiltSpeed_ = 0.5, zoomSpeed_ = 0.5;
};

}  // namespace hb::ptz
