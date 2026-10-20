#pragma once
#include <memory>
#include <mutex>

#include "ptz/controller.hpp"
#include "ptz/transport.hpp"
#include "ptz/visca.hpp"

namespace hb::ptz {

struct ViscaConfig {
    int address = 1;             // camera address 1..7 (serial); always 1 over IP
    bool overIp = true;          // true: VISCA over IP framing (8-byte header + sequence)
    Millis ackTimeout{500};      // per-command wait for ACK / inquiry reply
    int maxPresetId = 127;       // highest preset number accepted by the camera
    int maxReadsPerCommand = 8;  // stale/unrelated frames tolerated while waiting
    bool sequenceReset = true;   // send the IP "reset sequence number" control message on connect
};

// VISCA provider. Not validated on real hardware.
class ViscaProvider : public PTZProvider {
public:
    ViscaProvider(std::unique_ptr<ITransport> transport, ViscaConfig cfg = {});

    std::string name() const override { return cfg_.overIp ? "visca-ip" : "visca-serial"; }
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
    PtzResult command(const Bytes& payload);
    PtzResult inquiry(const Bytes& payload, visca::Message& reply);
    PtzResult exchange(const Bytes& payload, bool isInquiry, visca::Message* reply);
    PtzResult checkPreset(int id) const;

    std::unique_ptr<ITransport> t_;
    ViscaConfig cfg_;
    PTZCapabilities caps_;
    std::mutex mu_;
    std::uint32_t seq_ = 0;
    Bytes stream_;  // serial reassembly buffer
    bool connected_ = false;
    bool lensActive_ = false;
    std::optional<std::pair<double, double>> lastPT_;
    std::optional<double> lastZoom_;
};

}  // namespace hb::ptz
