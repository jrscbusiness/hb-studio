#pragma once
#include <chrono>
#include <optional>
#include <string>
#include <vector>

#include "onvif/http_client.hpp"
#include "onvif/messages.hpp"
#include "onvif/parsers.hpp"
#include "onvif/soap.hpp"

namespace hb::core { class Logger; }

namespace hb::onvif {

struct ClientOptions {
    std::string deviceServiceUrl;  // e.g. http://192.168.1.64/onvif/device_service
    Credentials credentials;
    std::chrono::milliseconds requestTimeout{5000};
    PasswordType passwordType = PasswordType::Digest;
    // Cameras behind NAT often advertise unreachable XAddrs. true = always use the host we connected to;
    // false (default) = only when the advertised host is 0.0.0.0/loopback.
    bool alwaysUseConnectHost = false;
    UnixClock clock = systemUnixClock();           // injectable
    NonceGenerator nonce = randomNonceGenerator();  // injectable
    hb::core::Logger* logger = nullptr;             // optional; messages never contain secrets
};

struct StreamChoice {
    MediaProfile profile;
    StreamUri stream;
};

struct ConnectionInfo {
    std::optional<DeviceInformation> device;
    ServiceEndpoints endpoints;
    std::int64_t clockOffsetSeconds = 0;
    bool clockSynced = false;
    std::vector<MediaProfile> profiles;
    StreamChoice main;                  // highest quality profile
    std::optional<StreamChoice> sub;    // lowest resolution profile, if distinct from main
    bool ptzSupported = false;          // PTZ service present AND main profile has a PTZ configuration
    std::vector<std::string> warnings;  // non-fatal problems
};

// Picks main (largest resolution; tie: better codec, higher bitrate) and sub (smallest resolution
// distinct from main). Pure function, exposed for tests. Profiles without video are ignored.
struct ProfileSelection {
    int main = -1;
    int sub = -1;
};
ProfileSelection selectProfiles(const std::vector<MediaProfile>& profiles);

class OnvifClient {
public:
    OnvifClient(IHttpClient& http, ClientOptions options);

    // connect -> GetSystemDateAndTime -> GetDeviceInformation -> GetCapabilities (fallback GetServices)
    //         -> GetProfiles -> GetStreamUri(main, sub). Only the failure of an essential step is an error.
    Result<ConnectionInfo> connect();

    // Individual steps (usable after connect() or standalone once endpoints are known).
    Result<SystemDateTime> syncClock();  // also stores the offset
    Result<DeviceInformation> getDeviceInformation();
    Result<ServiceEndpoints> getEndpoints();
    Result<std::vector<MediaProfile>> getProfiles();
    Result<StreamUri> getStreamUri(const std::string& profileToken);

    // PTZ (require endpoints.ptz; call connect() first or setEndpoints()).
    Result<Unit> ptzContinuousMove(const std::string& profileToken, double pan, double tilt, double zoom, double timeoutSeconds = 0);
    Result<Unit> ptzStop(const std::string& profileToken);
    Result<Unit> ptzGotoPreset(const std::string& profileToken, const std::string& presetToken);
    Result<std::string> ptzSetPreset(const std::string& profileToken, const std::string& name, const std::string& presetToken = {});
    Result<Unit> ptzRemovePreset(const std::string& profileToken, const std::string& presetToken);
    Result<std::vector<PtzPreset>> ptzGetPresets(const std::string& profileToken);
    Result<PtzStatus> ptzGetStatus(const std::string& profileToken);

    // Imaging (require endpoints.imaging)
    Result<ImagingSettings> imagingGetSettings(const std::string& videoSourceToken);
    Result<Unit> imagingMoveFocus(const std::string& videoSourceToken, double speed);
    Result<Unit> imagingStop(const std::string& videoSourceToken);

    void setEndpoints(const ServiceEndpoints& e) { endpoints_ = e; }
    const ServiceEndpoints& endpoints() const { return endpoints_; }
    void setClockOffsetSeconds(std::int64_t o) { soap_.setClockOffsetSeconds(o); }
    std::int64_t clockOffsetSeconds() const { return soap_.clockOffsetSeconds(); }

private:
    // Sends `msg` to `url`; returns the response body for 2xx, or a typed Error (401 -> Unauthorized,
    // SOAP fault in a 4xx/5xx -> fault error, else Http).
    Result<std::string> call(const std::string& url, const Message& msg);
    std::string fixXAddr(const std::string& xaddr) const;
    Result<std::string> requireEndpoint(const std::string& url, const char* what) const;
    void log(const std::string& text) const;

    IHttpClient& http_;
    ClientOptions opt_;
    SoapBuilder soap_;
    ServiceEndpoints endpoints_;
    std::string connectHost_;
};

}  // namespace hb::onvif
