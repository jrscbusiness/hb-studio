#include "ptz/provider_factory.hpp"
#include <algorithm>

#include "ptz/pelco_provider.hpp"
#include "ptz/udp_transport.hpp"
#include "ptz/visca_provider.hpp"

namespace hb::ptz {

std::unique_ptr<PTZProvider> makeProvider(const CameraPtzConfig& in, std::unique_ptr<ITransport> transport,
                                          std::shared_ptr<IOnvifPtzBackend> onvif) {
    try {
        CameraPtzConfig cfg = in;
        cfg.normalize();
        // Highest preset number on the wire for the configured numbering.
        const int maxId = cfg.presetBase + cfg.presetCount - 1;
        switch (cfg.protocol) {
            case PtzProtocol::OnvifPtz:
                if (!onvif) return nullptr;
                return std::make_unique<OnvifPtzProvider>(std::move(onvif));
            case PtzProtocol::ViscaIp: {
                if (!transport) {
                    if (cfg.host.empty()) return nullptr;
                    transport = makeUdpTransport(cfg.host, cfg.effectivePort());
                }
                ViscaConfig v;
                v.overIp = true;
                v.maxPresetId = std::max(maxId, 127);
                return std::make_unique<ViscaProvider>(std::move(transport), v);
            }
            case PtzProtocol::ViscaSerial: {
                if (!transport) return nullptr;
                ViscaConfig v;
                v.overIp = false;
                v.address = cfg.address;
                v.maxPresetId = std::max(maxId, 127);
                return std::make_unique<ViscaProvider>(std::move(transport), v);
            }
            case PtzProtocol::PelcoD:
            case PtzProtocol::PelcoP: {
                if (!transport) return nullptr;
                PelcoConfig p;
                p.variant = cfg.protocol == PtzProtocol::PelcoD ? PelcoVariant::D : PelcoVariant::P;
                p.address = cfg.address;
                p.maxPresetId = std::max(maxId, 255);
                return std::make_unique<PelcoProvider>(std::move(transport), p);
            }
        }
    } catch (...) {
    }
    return nullptr;
}

}  // namespace hb::ptz
