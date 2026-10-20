#pragma once
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "onvif/result.hpp"
#include "onvif/types.hpp"

namespace hb::onvif {

// All parsers take a full SOAP response body, are tolerant to namespace prefixes, missing optional
// fields and unknown elements, and NEVER throw: malformed input yields an Error.

Result<DeviceInformation> parseDeviceInformation(std::string_view xml);
Result<SystemDateTime> parseSystemDateAndTime(std::string_view xml);
Result<ServiceEndpoints> parseCapabilities(std::string_view xml);
Result<ServiceEndpoints> parseServices(std::string_view xml);
Result<std::vector<MediaProfile>> parseProfiles(std::string_view xml);
Result<StreamUri> parseStreamUri(std::string_view xml);
Result<std::vector<PtzPreset>> parsePresets(std::string_view xml);
Result<std::string> parseSetPresetResponse(std::string_view xml);  // preset token
Result<PtzStatus> parsePtzStatus(std::string_view xml);
Result<ImagingSettings> parseImagingSettings(std::string_view xml);
// For operations whose response has no data (Stop, GotoPreset, ...): just checks for faults.
Result<Unit> parseAck(std::string_view xml);

// Convenience for the HTTP layer: if `xml` is a SOAP Fault, returns it as an Error.
std::optional<Error> parseFaultOnly(std::string_view xml);

}  // namespace hb::onvif
