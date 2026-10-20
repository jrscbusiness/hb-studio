#include "onvif/onvif_client.hpp"

#include <algorithm>

#include "core/logger.hpp"
#include "onvif/url.hpp"

namespace hb::onvif {
namespace {

long long pixels(const MediaProfile& p) {
    return p.video ? static_cast<long long>(p.video->width) * p.video->height : 0;
}

int codecRank(const MediaProfile& p) {
    if (!p.video) return 0;
    const auto& e = p.video->encoding;
    if (e == "H265") return 4;
    if (e == "H264") return 3;
    if (e == "MPEG4") return 2;
    if (e == "JPEG") return 1;
    return 0;
}

}  // namespace

ProfileSelection selectProfiles(const std::vector<MediaProfile>& profiles) {
    ProfileSelection sel;
    if (profiles.empty()) return sel;
    auto better = [&](int a, int b) {  // is a a better "main" than b?
        const auto &pa = profiles[a], &pb = profiles[b];
        if (pixels(pa) != pixels(pb)) return pixels(pa) > pixels(pb);
        if (codecRank(pa) != codecRank(pb)) return codecRank(pa) > codecRank(pb);
        const int ba = pa.video ? pa.video->bitrateKbps : 0, bb = pb.video ? pb.video->bitrateKbps : 0;
        return ba > bb;
    };
    sel.main = 0;
    for (int i = 1; i < static_cast<int>(profiles.size()); ++i)
        if (better(i, sel.main)) sel.main = i;
    const long long mainPx = pixels(profiles[sel.main]);
    for (int i = 0; i < static_cast<int>(profiles.size()); ++i) {
        if (i == sel.main || !profiles[i].video || pixels(profiles[i]) >= mainPx) continue;
        if (sel.sub < 0) { sel.sub = i; continue; }
        const bool modernI = codecRank(profiles[i]) >= 3, modernS = codecRank(profiles[sel.sub]) >= 3;
        if (modernI != modernS ? modernI : pixels(profiles[i]) < pixels(profiles[sel.sub])) sel.sub = i;
    }
    return sel;
}

OnvifClient::OnvifClient(IHttpClient& http, ClientOptions options)
    : http_(http), opt_(std::move(options)), soap_(opt_.clock, opt_.nonce) {
    soap_.setPasswordType(opt_.passwordType);
    endpoints_.device = opt_.deviceServiceUrl;
    if (auto u = parseUrl(opt_.deviceServiceUrl); u.ok()) connectHost_ = u.value().host;
}

void OnvifClient::log(const std::string& text) const {
    if (opt_.logger) opt_.logger->debug("onvif", text);  // Logger redacts secrets again
}

std::string OnvifClient::fixXAddr(const std::string& xaddr) const {
    if (xaddr.empty() || connectHost_.empty()) return xaddr;
    auto u = parseUrl(xaddr);
    if (!u.ok()) return xaddr;
    if (opt_.alwaysUseConnectHost || isUnroutableHost(u.value().host)) return replaceUrlHost(xaddr, connectHost_);
    return xaddr;
}

Result<std::string> OnvifClient::requireEndpoint(const std::string& url, const char* what) const {
    if (url.empty()) return makeError(ErrorCode::Unsupported, std::string("device has no ") + what + " service");
    return url;
}

Result<std::string> OnvifClient::call(const std::string& url, const Message& msg) {
    if (url.empty()) return makeError(ErrorCode::InvalidArgument, "empty service URL");
    HttpRequest req;
    req.method = "POST";
    req.url = url;
    req.timeout = opt_.requestTimeout;
    req.headers.emplace_back("Content-Type", soapContentType(msg.action));
    req.body = soap_.build(msg, &opt_.credentials);
    {
        std::string shown = url;
        if (auto u = parseUrl(url); u.ok()) shown = urlToString(u.value());  // strips userinfo
        log("POST " + shown + " " + msg.action);
    }
    auto res = http_.send(req);
    if (!res.ok()) return res.error();
    const HttpResponse& r = res.value();
    if (r.status >= 200 && r.status < 300) return r.body;
    Error e;
    if (auto fault = parseFaultOnly(r.body)) {
        e = *fault;
    } else if (r.status == 401 || r.status == 403) {
        e = makeError(ErrorCode::Unauthorized, "HTTP " + std::to_string(r.status));
    } else {
        e = makeError(ErrorCode::Http, "HTTP " + std::to_string(r.status));
    }
    if ((r.status == 401 || r.status == 403) && e.code != ErrorCode::Unauthorized) e.code = ErrorCode::Unauthorized;
    e.httpStatus = r.status;
    return e;
}

#define HB_TRY_CALL(var, url, msg)                  \
    auto var = call((url), (msg));                  \
    if (!var.ok()) return var.error()

Result<SystemDateTime> OnvifClient::syncClock() {
    HB_TRY_CALL(body, endpoints_.device, messages::getSystemDateAndTime());
    auto dt = parseSystemDateAndTime(body.value());
    if (dt.ok()) soap_.setClockOffsetSeconds(dt.value().utcSeconds - opt_.clock());
    return dt;
}

Result<DeviceInformation> OnvifClient::getDeviceInformation() {
    HB_TRY_CALL(body, endpoints_.device, messages::getDeviceInformation());
    return parseDeviceInformation(body.value());
}

Result<ServiceEndpoints> OnvifClient::getEndpoints() {
    ServiceEndpoints result;
    std::optional<Error> firstError;
    auto body = call(endpoints_.device, messages::getCapabilities());
    if (body.ok()) {
        auto caps = parseCapabilities(body.value());
        if (caps.ok()) result = caps.value();
        else firstError = caps.error();
    } else {
        firstError = body.error();
    }
    if (firstError && firstError->code == ErrorCode::Unauthorized) return *firstError;
    if (result.media.empty()) {  // Capabilities failed or lacks Media: try GetServices
        auto sb = call(endpoints_.device, messages::getServices(false));
        if (sb.ok()) {
            auto svc = parseServices(sb.value());
            if (svc.ok()) {
                const auto& s = svc.value();
                if (result.media.empty()) result.media = s.media;
                if (result.ptz.empty()) result.ptz = s.ptz;
                if (result.imaging.empty()) result.imaging = s.imaging;
                if (result.events.empty()) result.events = s.events;
                if (result.device.empty()) result.device = s.device;
            } else if (!firstError) {
                firstError = svc.error();
            }
        } else if (!firstError) {
            firstError = sb.error();
        }
    }
    if (result.media.empty() && result.ptz.empty() && result.imaging.empty())
        return firstError ? *firstError : makeError(ErrorCode::Unsupported, "device advertises no usable service");
    result.device = opt_.deviceServiceUrl;  // always keep the URL we were given for the device service
    result.media = fixXAddr(result.media);
    result.ptz = fixXAddr(result.ptz);
    result.imaging = fixXAddr(result.imaging);
    result.events = fixXAddr(result.events);
    return result;
}

Result<std::vector<MediaProfile>> OnvifClient::getProfiles() {
    auto url = requireEndpoint(endpoints_.media, "Media");
    if (!url.ok()) return url.error();
    HB_TRY_CALL(body, url.value(), messages::getProfiles());
    return parseProfiles(body.value());
}

Result<StreamUri> OnvifClient::getStreamUri(const std::string& profileToken) {
    auto url = requireEndpoint(endpoints_.media, "Media");
    if (!url.ok()) return url.error();
    if (profileToken.empty()) return makeError(ErrorCode::InvalidArgument, "empty profile token");
    HB_TRY_CALL(body, url.value(), messages::getStreamUri(profileToken));
    auto uri = parseStreamUri(body.value());
    if (uri.ok() && !connectHost_.empty()) {
        if (auto u = parseUrl(uri.value().uri); u.ok() && (opt_.alwaysUseConnectHost || isUnroutableHost(u.value().host)))
            uri.value().uri = replaceUrlHost(uri.value().uri, connectHost_);
    }
    return uri;
}

Result<ConnectionInfo> OnvifClient::connect() {
    ConnectionInfo info;
    if (auto u = parseUrl(opt_.deviceServiceUrl); !u.ok()) return u.error();

    // 1. Clock offset. Failure is not fatal (some devices block it), but is the usual cause of later auth errors.
    auto dt = syncClock();
    if (dt.ok()) {
        info.clockSynced = true;
        info.clockOffsetSeconds = soap_.clockOffsetSeconds();
    } else {
        info.warnings.push_back("GetSystemDateAndTime failed (" + dt.error().message + "); clock offset assumed 0");
    }

    // 2. Device information (informational; auth failure here is definitive).
    auto di = getDeviceInformation();
    if (di.ok()) info.device = di.value();
    else if (di.error().code == ErrorCode::Unauthorized) return di.error();
    else info.warnings.push_back("GetDeviceInformation failed: " + di.error().message);

    // 3. Services.
    auto eps = getEndpoints();
    if (!eps.ok()) return eps.error();
    endpoints_ = eps.value();
    info.endpoints = endpoints_;
    if (endpoints_.media.empty()) return makeError(ErrorCode::Unsupported, "device has no Media service");

    // 4. Profiles.
    auto profiles = getProfiles();
    if (!profiles.ok()) return profiles.error();
    info.profiles = profiles.value();

    // 5. Stream URIs.
    const auto sel = selectProfiles(info.profiles);
    if (sel.main < 0) return makeError(ErrorCode::MissingField, "no media profile");
    info.main.profile = info.profiles[static_cast<std::size_t>(sel.main)];
    auto mainUri = getStreamUri(info.main.profile.token);
    if (!mainUri.ok()) return mainUri.error();
    info.main.stream = mainUri.value();
    if (sel.sub >= 0) {
        StreamChoice sub;
        sub.profile = info.profiles[static_cast<std::size_t>(sel.sub)];
        auto subUri = getStreamUri(sub.profile.token);
        if (subUri.ok()) {
            sub.stream = subUri.value();
            info.sub = std::move(sub);
        } else {
            info.warnings.push_back("GetStreamUri for sub profile failed: " + subUri.error().message);
        }
    } else {
        info.warnings.push_back("no lower-resolution profile found: sub-stream unavailable");
    }
    info.ptzSupported = !endpoints_.ptz.empty() && info.main.profile.hasPtz();
    return info;
}

// ---- PTZ ----

Result<Unit> OnvifClient::ptzContinuousMove(const std::string& t, double pan, double tilt, double zoom, double timeoutSeconds) {
    auto url = requireEndpoint(endpoints_.ptz, "PTZ");
    if (!url.ok()) return url.error();
    HB_TRY_CALL(body, url.value(), messages::ptzContinuousMove(t, pan, tilt, zoom, timeoutSeconds));
    return parseAck(body.value());
}

Result<Unit> OnvifClient::ptzStop(const std::string& t) {
    auto url = requireEndpoint(endpoints_.ptz, "PTZ");
    if (!url.ok()) return url.error();
    HB_TRY_CALL(body, url.value(), messages::ptzStop(t));
    return parseAck(body.value());
}

Result<Unit> OnvifClient::ptzGotoPreset(const std::string& t, const std::string& preset) {
    auto url = requireEndpoint(endpoints_.ptz, "PTZ");
    if (!url.ok()) return url.error();
    HB_TRY_CALL(body, url.value(), messages::ptzGotoPreset(t, preset));
    return parseAck(body.value());
}

Result<std::string> OnvifClient::ptzSetPreset(const std::string& t, const std::string& name, const std::string& preset) {
    auto url = requireEndpoint(endpoints_.ptz, "PTZ");
    if (!url.ok()) return url.error();
    HB_TRY_CALL(body, url.value(), messages::ptzSetPreset(t, name, preset));
    return parseSetPresetResponse(body.value());
}

Result<Unit> OnvifClient::ptzRemovePreset(const std::string& t, const std::string& preset) {
    auto url = requireEndpoint(endpoints_.ptz, "PTZ");
    if (!url.ok()) return url.error();
    HB_TRY_CALL(body, url.value(), messages::ptzRemovePreset(t, preset));
    return parseAck(body.value());
}

Result<std::vector<PtzPreset>> OnvifClient::ptzGetPresets(const std::string& t) {
    auto url = requireEndpoint(endpoints_.ptz, "PTZ");
    if (!url.ok()) return url.error();
    HB_TRY_CALL(body, url.value(), messages::ptzGetPresets(t));
    return parsePresets(body.value());
}

Result<PtzStatus> OnvifClient::ptzGetStatus(const std::string& t) {
    auto url = requireEndpoint(endpoints_.ptz, "PTZ");
    if (!url.ok()) return url.error();
    HB_TRY_CALL(body, url.value(), messages::ptzGetStatus(t));
    return parsePtzStatus(body.value());
}

// ---- Imaging ----

Result<ImagingSettings> OnvifClient::imagingGetSettings(const std::string& src) {
    auto url = requireEndpoint(endpoints_.imaging, "Imaging");
    if (!url.ok()) return url.error();
    HB_TRY_CALL(body, url.value(), messages::imagingGetSettings(src));
    return parseImagingSettings(body.value());
}

Result<Unit> OnvifClient::imagingMoveFocus(const std::string& src, double speed) {
    auto url = requireEndpoint(endpoints_.imaging, "Imaging");
    if (!url.ok()) return url.error();
    HB_TRY_CALL(body, url.value(), messages::imagingMoveFocus(src, speed));
    return parseAck(body.value());
}

Result<Unit> OnvifClient::imagingStop(const std::string& src) {
    auto url = requireEndpoint(endpoints_.imaging, "Imaging");
    if (!url.ok()) return url.error();
    HB_TRY_CALL(body, url.value(), messages::imagingStop(src));
    return parseAck(body.value());
}

}  // namespace hb::onvif
