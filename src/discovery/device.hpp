#pragma once
#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace hb::discovery {

enum class Protocol { Onvif, Ndi, Uvc, Rtsp };
const char* protocolName(Protocol p);

enum class DeviceState {
    Discovered,           // usable as found
    RequiresCredentials,  // ONVIF device: GetProfiles needs a login
    Unreachable,
};
const char* stateName(DeviceState s);

// Normalised result shared by every discovery source (see ARCHITECTURE.md, section 7).
struct DiscoveredDevice {
    std::string id;            // stable: "onvif:<endpoint-uuid>" or "<protocol>:<ip>"
    std::string name;
    std::string manufacturer;  // heuristic (from ONVIF scopes); may be empty
    std::string ip;
    std::vector<Protocol> protocols;
    bool ptz = false;          // hint only; confirmed after ONVIF connect
    DeviceState state = DeviceState::Discovered;
    std::vector<std::string> xaddrs;  // ONVIF device-service URLs
    std::string source;        // name of the first source that found it
};

// Cooperative cancellation: the engine sets it when a source exceeds its timeout.
class CancelToken {
public:
    CancelToken() : flag_(std::make_shared<std::atomic<bool>>(false)) {}
    void cancel() const { flag_->store(true); }
    bool cancelled() const { return flag_->load(); }

private:
    std::shared_ptr<std::atomic<bool>> flag_;
};

struct SourceResult {
    std::vector<DiscoveredDevice> devices;
    std::string error;  // non-empty => failure (devices found so far may still be present)
    bool ok() const { return error.empty(); }
};

// A discovery mechanism (ONVIF WS-Discovery now; NDI/UVC later). discover() may block up to its own
// listen time and must poll `cancel`. It runs on an engine-owned thread and may throw; the engine isolates it.
class IDiscoverySource {
public:
    virtual ~IDiscoverySource() = default;
    virtual std::string name() const = 0;
    virtual SourceResult discover(const CancelToken& cancel) = 0;
};

}  // namespace hb::discovery
