#pragma once
#include <memory>

#include "ptz/camera_config.hpp"
#include "ptz/controller.hpp"
#include "ptz/onvif_provider.hpp"
#include "ptz/transport.hpp"

namespace hb::ptz {

// Builds the provider described by a per-camera config.
//  - `transport`: injected transport (serial, fake...). If null, VISCA IP gets a real UDP transport
//    to host:effectivePort(); serial and Pelco need an injected transport.
//  - `onvif`: backend from the onvif module (required for PtzProtocol::OnvifPtz).
// Returns nullptr when the config cannot be satisfied (PTZ stays disabled; video is unaffected).
std::unique_ptr<PTZProvider> makeProvider(const CameraPtzConfig& cfg, std::unique_ptr<ITransport> transport,
                                          std::shared_ptr<IOnvifPtzBackend> onvif);

}  // namespace hb::ptz
