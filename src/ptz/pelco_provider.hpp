#pragma once
#include <memory>
#include <mutex>

#include "ptz/controller.hpp"
#include "ptz/pelco.hpp"
#include "ptz/transport.hpp"

namespace hb::ptz {

enum class PelcoVariant { D, P };

struct PelcoConfig {
    PelcoVariant variant = PelcoVariant::D;
    int address = 1;  // D: 1..255, P: 0..31
    int maxPresetId = 255;
};

// Pelco-D/P provider. One-way protocol: success means "bytes handed to the transport".
// Not validated on real hardware.
class PelcoProvider : public PTZProvider {
public:
    PelcoProvider(std::unique_ptr<ITransport> transport, PelcoConfig cfg = {});

    std::string name() const override { return cfg_.variant == PelcoVariant::D ? "pelco-d" : "pelco-p"; }
    const PTZCapabilities& capabilities() const override { return caps_; }

    PtzResult connect() override;
    PtzResult disconnect() override;
    PtzResult drive(const MoveVector& v) override;
    PtzResult stopLens() override;

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
    PtzResult sendCmd(const pelco::Command& c);
    PtzResult checkPreset(int id) const;

    std::unique_ptr<ITransport> t_;
    PelcoConfig cfg_;
    PTZCapabilities caps_;
    std::mutex mu_;
    bool connected_ = false;
    bool lensActive_ = false;
};

}  // namespace hb::ptz
