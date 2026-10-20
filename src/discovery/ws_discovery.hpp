#pragma once
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "onvif/result.hpp"

namespace hb::discovery {

inline constexpr const char* kWsDiscoveryGroup = "239.255.255.250";
inline constexpr int kWsDiscoveryPort = 3702;

// Probe for dn:NetworkVideoTransmitter. `messageId` is "uuid:...".
std::string buildProbe(std::string_view messageId);

struct ProbeMatch {
    std::string endpointAddress;  // wsa:EndpointReference/Address, usually "urn:uuid:..."
    std::vector<std::string> types;
    std::vector<std::string> scopes;
    std::vector<std::string> xaddrs;
    std::string relatesTo;        // header RelatesTo (the Probe MessageID)
    int metadataVersion = 0;
};

// Tolerant to prefixes; a malformed datagram yields an Error (never throws). Returns every ProbeMatch.
hb::onvif::Result<std::vector<ProbeMatch>> parseProbeMatches(std::string_view xml);

struct ScopeInfo {
    std::string name;      // onvif://www.onvif.org/name/<x>, percent-decoded
    std::string hardware;  // .../hardware/<x>
    std::string location;  // .../location/<x>
    std::vector<std::string> profiles;  // "S", "T", "G"...
    bool ptzHint = false;  // scope .../type/ptz present
};

ScopeInfo interpretScopes(const std::vector<std::string>& scopes);
std::string percentDecode(std::string_view s);

// True if every prefix in `required` is a (case-insensitive) prefix of at least one scope.
// Example prefixes: "onvif://www.onvif.org/name/", "onvif://www.onvif.org/hardware/DS-2".
bool matchesScopes(const std::vector<std::string>& scopes, const std::vector<std::string>& required);

// Best IPv4 host among the XAddrs (skips 169.254.x.x unless nothing else); falls back to any host.
std::string ipFromXAddrs(const std::vector<std::string>& xaddrs);

// Heuristic vendor guess from the ONVIF name/hardware scope text. Empty if unknown.
std::string guessManufacturer(std::string_view nameOrHardware);

}  // namespace hb::discovery
