#pragma once
// ONVIF PTZ provider. It only talks to IOnvifPtzBackend; the SOAP implementation
// lives in the onvif module (another owner). Speeds are normalized [-1,1], exactly
// what ONVIF ContinuousMove expects.
#include <memory>
#include <mutex>

#include "ptz/controller.hpp"

namespace hb::ptz {

struct OnvifPtzBackendCaps {
    bool ptz = true;
    bool absoluteMove = false;
    bool presets = true;       // GotoPreset / SetPreset / RemovePreset / GetPresets
    bool imagingFocus = false; // Imaging Move (continuous focus)
    bool autoFocus = false;    // Imaging SetImagingSettings focus mode
    bool iris = false;
    bool status = true;        // PTZ GetStatus
    int maxPresets = 64;
};

class IOnvifPtzBackend {
public:
    virtual ~IOnvifPtzBackend() = default;
    virtual PtzResult connect() = 0;
    virtual PtzResult disconnect() = 0;
    virtual OnvifPtzBackendCaps capabilities() const = 0;

    virtual PtzResult continuousMove(double pan, double tilt, double zoom) = 0;  // each [-1,1]
    virtual PtzResult stop(bool panTilt, bool zoom) = 0;
    virtual PtzResult absoluteMove(const PtzPosition& normalized) = 0;
    virtual PtzResult gotoPreset(int id) = 0;
    virtual PtzResult setPreset(int id, const std::string& name) = 0;
    virtual PtzResult removePreset(int id) = 0;
    virtual PtzReply<std::vector<PresetInfo>> getPresets() = 0;
    virtual PtzResult focusMove(double speed) = 0;  // <0 near, >0 far
    virtual PtzResult focusStop() = 0;
    virtual PtzResult setFocusAuto(bool autoMode) = 0;
    virtual PtzResult irisStep(int direction) = 0;  // +1 open, -1 close
    virtual PtzReply<PtzPosition> getPosition() = 0;
};

class OnvifPtzProvider : public PTZProvider {
public:
    explicit OnvifPtzProvider(std::shared_ptr<IOnvifPtzBackend> backend);

    std::string name() const override { return "onvif-ptz"; }
    const PTZCapabilities& capabilities() const override { return caps_; }

    PtzResult connect() override;
    PtzResult disconnect() override;
    PtzResult drive(const MoveVector& v) override;
    PtzResult stopAll() override;
    PtzResult stopLens() override;
    PtzResult moveAbsolute(const PtzPosition& p) override;

    PtzResult gotoPreset(int id) override;
    PtzResult savePreset(int id) override;
    PtzResult deletePreset(int id) override;
    PtzReply<std::vector<PresetInfo>> getPresets() override;

    PtzResult focusAuto() override;
    PtzResult focusManual() override;
    PtzResult focusNear() override;
    PtzResult focusFar() override;
    PtzResult irisOpen() override;
    PtzResult irisClose() override;
    PtzReply<CameraStatus> getCameraStatus() override;

private:
    void refreshCaps();

    std::shared_ptr<IOnvifPtzBackend> b_;
    PTZCapabilities caps_;
    OnvifPtzBackendCaps bcaps_;
    std::mutex mu_;
    bool connected_ = false;
    bool lensActive_ = false;
};

}  // namespace hb::ptz
