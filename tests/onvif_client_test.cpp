#include <gtest/gtest.h>

#include <functional>
#include <map>

#include <pugixml.hpp>

#include "onvif/crypto.hpp"
#include "onvif/onvif_client.hpp"
#include "onvif_fixtures.hpp"

using namespace hb::onvif;

namespace {

// Scripted HTTP fake: maps the operation name (last path of the SOAPAction) to a response.
class FakeHttp : public IHttpClient {
public:
    struct Reply {
        int status = 200;
        std::string body;
    };
    std::map<std::string, Reply> replies;                // key: operation name
    std::map<std::string, Error> transportErrors;        // key: operation name
    std::vector<HttpRequest> requests;
    std::vector<std::string> ops;

    static std::string opOf(const HttpRequest& r) {
        for (const auto& h : r.headers) {
            if (h.first != "Content-Type") continue;
            const auto a = h.second.find("action=\"");
            const auto e = h.second.rfind('"');
            const std::string action = h.second.substr(a + 8, e - a - 8);
            return action.substr(action.rfind('/') + 1);
        }
        return {};
    }

    Result<HttpResponse> send(const HttpRequest& r) override {
        requests.push_back(r);
        const std::string op = opOf(r);
        ops.push_back(op);
        if (auto t = transportErrors.find(op); t != transportErrors.end()) return t->second;
        auto it = replies.find(op);
        if (it == replies.end()) return HttpResponse{404, "", {}};
        return HttpResponse{it->second.status, it->second.body, {}};
    }
};

ClientOptions baseOptions(std::int64_t localNow) {
    ClientOptions o;
    o.deviceServiceUrl = "http://192.168.1.64/onvif/device_service";
    o.credentials = {"admin", "S3cret!"};
    o.clock = [localNow] { return localNow; };
    o.nonce = [] { return std::string("0123456789abcdef"); };
    return o;
}

FakeHttp hikCamera() {
    FakeHttp h;
    h.replies["GetSystemDateAndTime"] = {200, fx::kHikDateTime};
    h.replies["GetDeviceInformation"] = {200, fx::kHikDeviceInfo};
    h.replies["GetCapabilities"] = {200, fx::kHikCapabilities};
    h.replies["GetProfiles"] = {200, fx::kHikProfiles};
    h.replies["GetStreamUri"] = {200, fx::kHikStreamUriMain};
    return h;
}

// GetStreamUri replies depend on the requested profile.
class HikHttp : public FakeHttp {
public:
    Result<HttpResponse> send(const HttpRequest& r) override {
        if (opOf(r) == "GetStreamUri") {
            requests.push_back(r);
            ops.push_back("GetStreamUri");
            const bool sub = r.body.find("Profile_2") != std::string::npos;
            return HttpResponse{200, sub ? fx::kHikStreamUriSub : fx::kHikStreamUriMain, {}};
        }
        return FakeHttp::send(r);
    }
};

std::string authCreated(const std::string& envelope) {
    pugi::xml_document d;
    d.load_string(envelope.c_str());
    return d.child("s:Envelope").child("s:Header").child("wsse:Security").child("wsse:UsernameToken").child("wsu:Created").text().get();
}

}  // namespace

TEST(Client, FullConnectFlowHikvision) {
    HikHttp http;
    http.replies = hikCamera().replies;
    OnvifClient c(http, baseOptions(fx::kHikDateTimeUnix - 1000));  // local clock is 1000 s behind the camera
    auto r = c.connect();
    ASSERT_TRUE(r.ok()) << r.error().message;
    const auto& info = r.value();

    EXPECT_TRUE(info.clockSynced);
    EXPECT_EQ(info.clockOffsetSeconds, 1000);
    ASSERT_TRUE(info.device.has_value());
    EXPECT_EQ(info.device->manufacturer, "HIKVISION");
    EXPECT_EQ(info.endpoints.media, "http://192.168.1.64/onvif/Media");
    EXPECT_EQ(info.main.profile.token, "Profile_1");
    EXPECT_EQ(info.main.stream.uri.find("rtsp://192.168.1.64:554/Streaming/Channels/101"), 0u);
    ASSERT_TRUE(info.sub.has_value());
    EXPECT_EQ(info.sub->profile.token, "Profile_2");
    EXPECT_NE(info.sub->stream.uri.find("Channels/102"), std::string::npos);
    EXPECT_TRUE(info.ptzSupported);
    EXPECT_TRUE(info.warnings.empty());

    // Order of the exchange.
    const std::vector<std::string> expected = {"GetSystemDateAndTime", "GetDeviceInformation", "GetCapabilities",
                                               "GetProfiles",           "GetStreamUri",         "GetStreamUri"};
    EXPECT_EQ(http.ops, expected);
    // Media calls go to the media XAddr; the clock request is unauthenticated.
    EXPECT_EQ(http.requests[3].url, "http://192.168.1.64/onvif/Media");
    EXPECT_EQ(http.requests[0].body.find("Security"), std::string::npos);
    EXPECT_NE(http.requests[1].body.find("Security"), std::string::npos);
    // Created = local now (+ offset) = the camera's time, and the password never travels in clear.
    EXPECT_EQ(authCreated(http.requests[1].body), "2024-03-07T13:05:09Z");
    for (const auto& q : http.requests) EXPECT_EQ(q.body.find("S3cret!"), std::string::npos);
}

TEST(Client, AuthDigestUsesCorrectedClock) {
    // Without correction Created would be 1000 s earlier: prove the offset reaches the digest.
    HikHttp http;
    http.replies = hikCamera().replies;
    auto opt = baseOptions(fx::kHikDateTimeUnix - 1000);
    OnvifClient c(http, opt);
    ASSERT_TRUE(c.syncClock().ok());
    EXPECT_EQ(c.clockOffsetSeconds(), 1000);
    ASSERT_TRUE(c.getDeviceInformation().ok());
    pugi::xml_document d;
    d.load_string(http.requests.back().body.c_str());
    auto tok = d.child("s:Envelope").child("s:Header").child("wsse:Security").child("wsse:UsernameToken");
    EXPECT_EQ(std::string(tok.child("wsse:Password").text().get()),
              passwordDigest("0123456789abcdef", "2024-03-07T13:05:09Z", "S3cret!"));
}

TEST(Client, UnauthorizedIsTypedAndStopsConnect) {
    FakeHttp http = hikCamera();
    http.replies["GetDeviceInformation"] = {400, fx::kHikNotAuthorized};
    OnvifClient c(http, baseOptions(0));
    auto r = c.connect();
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, ErrorCode::Unauthorized);
    EXPECT_EQ(r.error().httpStatus, 400);
    EXPECT_EQ(http.ops.back(), "GetDeviceInformation");  // did not continue
}

TEST(Client, Http401WithoutBodyIsUnauthorized) {
    FakeHttp http = hikCamera();
    http.replies["GetDeviceInformation"] = {401, "Unauthorized"};
    OnvifClient c(http, baseOptions(0));
    auto r = c.connect();
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, ErrorCode::Unauthorized);
}

TEST(Client, ClockFailureIsAWarningNotFatal) {
    HikHttp http;
    http.replies = hikCamera().replies;
    http.replies["GetSystemDateAndTime"] = {500, "boom"};
    OnvifClient c(http, baseOptions(100));
    auto r = c.connect();
    ASSERT_TRUE(r.ok());
    EXPECT_FALSE(r.value().clockSynced);
    ASSERT_EQ(r.value().warnings.size(), 1u);
    EXPECT_NE(r.value().warnings[0].find("clock offset assumed 0"), std::string::npos);
}

TEST(Client, FallsBackToGetServicesAndSelectsDahuaProfiles) {
    FakeHttp http;
    http.replies["GetSystemDateAndTime"] = {200, fx::kHikDateTime};
    http.replies["GetDeviceInformation"] = {200, fx::kHikDeviceInfo};
    http.replies["GetCapabilities"] = {500, "<html>Internal error</html>"};
    http.replies["GetServices"] = {200, fx::kDahuaServices};
    http.replies["GetProfiles"] = {200, fx::kDahuaProfiles};
    http.replies["GetStreamUri"] = {200, fx::kDahuaStreamUri};
    auto opt = baseOptions(0);
    opt.deviceServiceUrl = "http://192.168.1.108/onvif/device_service";
    OnvifClient c(http, opt);
    auto r = c.connect();
    ASSERT_TRUE(r.ok()) << r.error().message;
    EXPECT_EQ(r.value().endpoints.media, "http://192.168.1.108/onvif/media_service");
    EXPECT_EQ(r.value().main.profile.token, "MediaProfile00000");  // 2688x1520 H265
    ASSERT_TRUE(r.value().sub.has_value());
    EXPECT_EQ(r.value().sub->profile.token, "MediaProfile00001");  // 704x576 H264 preferred over smaller MJPEG
    EXPECT_FALSE(r.value().ptzSupported);  // service present but no profile has a PTZ configuration
}

TEST(Client, NatedXAddrsAreRewrittenToConnectHost) {
    std::string caps = fx::kHikCapabilities;
    for (std::size_t p; (p = caps.find("192.168.1.64")) != std::string::npos;) caps.replace(p, 12, "0.0.0.0");
    HikHttp http;
    http.replies = hikCamera().replies;
    http.replies["GetCapabilities"] = {200, caps};
    OnvifClient c(http, baseOptions(0));
    auto r = c.connect();
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value().endpoints.media, "http://192.168.1.64/onvif/Media");
    EXPECT_EQ(http.requests[3].url, "http://192.168.1.64/onvif/Media");
}

TEST(Client, AlwaysUseConnectHostOption) {
    HikHttp http;
    http.replies = hikCamera().replies;
    auto opt = baseOptions(0);
    opt.deviceServiceUrl = "http://10.9.9.9:8080/onvif/device_service";  // e.g. port forward / different route
    opt.alwaysUseConnectHost = true;
    OnvifClient c(http, opt);
    auto r = c.connect();
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value().endpoints.media, "http://10.9.9.9/onvif/Media");
    EXPECT_NE(r.value().main.stream.uri.find("rtsp://10.9.9.9:554/"), std::string::npos);
}

TEST(Client, TransportFailureAndBadUrl) {
    FakeHttp http;
    http.transportErrors["GetSystemDateAndTime"] = makeError(ErrorCode::Timeout, "timeout");
    http.transportErrors["GetDeviceInformation"] = makeError(ErrorCode::Timeout, "timeout");
    http.transportErrors["GetCapabilities"] = makeError(ErrorCode::Timeout, "timeout");
    http.transportErrors["GetServices"] = makeError(ErrorCode::Timeout, "timeout");
    OnvifClient c(http, baseOptions(0));
    auto r = c.connect();
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, ErrorCode::Timeout);

    auto bad = baseOptions(0);
    bad.deviceServiceUrl = "not a url";
    OnvifClient c2(http, bad);
    EXPECT_EQ(c2.connect().error().code, ErrorCode::InvalidArgument);
}

TEST(Client, GarbageResponsesDoNotCrash) {
    FakeHttp http;
    for (const char* op : {"GetSystemDateAndTime", "GetDeviceInformation", "GetCapabilities", "GetServices", "GetProfiles"})
        http.replies[op] = {200, "\x01\x02<<<not xml"};
    OnvifClient c(http, baseOptions(0));
    EXPECT_FALSE(c.connect().ok());
}

TEST(Client, SingleProfileHasNoSubStream) {
    FakeHttp http = hikCamera();
    http.replies["GetProfiles"] = {200, fx::kGenericProfilesDefaultNs};
    OnvifClient c(http, baseOptions(0));
    auto r = c.connect();
    ASSERT_TRUE(r.ok());
    EXPECT_FALSE(r.value().sub.has_value());
    EXPECT_EQ(r.value().main.profile.token, "p0");
}

TEST(ProfileSelection, AxisPrefersH264OverJpegAtSameResolution) {
    auto p = parseProfiles(fx::kAxisProfiles);
    ASSERT_TRUE(p.ok());
    const auto sel = selectProfiles(p.value());
    ASSERT_GE(sel.main, 0);
    EXPECT_EQ(p.value()[static_cast<std::size_t>(sel.main)].token, "profile_2_h264");
    ASSERT_GE(sel.sub, 0);
    EXPECT_EQ(p.value()[static_cast<std::size_t>(sel.sub)].token, "profile_3_h264_low");
    EXPECT_EQ(selectProfiles({}).main, -1);
}

TEST(ClientPtz, CommandsReachPtzServiceAndParse) {
    FakeHttp http;
    http.replies["ContinuousMove"] = {200, fx::kPtzStopResponse};
    http.replies["Stop"] = {200, fx::kPtzStopResponse};
    http.replies["GotoPreset"] = {200, fx::kPtzStopResponse};
    http.replies["RemovePreset"] = {200, fx::kPtzStopResponse};
    http.replies["SetPreset"] = {200, fx::kSetPresetResponse};
    http.replies["GetPresets"] = {200, fx::kPresets};
    http.replies["GetStatus"] = {200, fx::kPtzStatus};
    http.replies["GetImagingSettings"] = {200, fx::kImagingSettings};
    http.replies["Move"] = {200, fx::kPtzStopResponse};
    OnvifClient c(http, baseOptions(1284623445));
    ServiceEndpoints e;
    e.ptz = "http://192.168.1.64/onvif/PTZ";
    e.imaging = "http://192.168.1.64/onvif/Imaging";
    c.setEndpoints(e);

    EXPECT_TRUE(c.ptzContinuousMove("Profile_1", 0.3, -0.2, 0, 1.0).ok());
    EXPECT_NE(http.requests.back().body.find("x=\"0.3000\" y=\"-0.2000\""), std::string::npos);
    EXPECT_NE(http.requests.back().body.find("Security"), std::string::npos);
    EXPECT_TRUE(c.ptzStop("Profile_1").ok());
    EXPECT_TRUE(c.ptzGotoPreset("Profile_1", "1").ok());
    EXPECT_EQ(c.ptzSetPreset("Profile_1", "Altar").value(), "7");
    EXPECT_TRUE(c.ptzRemovePreset("Profile_1", "7").ok());
    EXPECT_EQ(c.ptzGetPresets("Profile_1").value().size(), 3u);
    EXPECT_DOUBLE_EQ(*c.ptzGetStatus("Profile_1").value().pan, -0.25);
    EXPECT_EQ(c.imagingGetSettings("VideoSource_1").value().focusMode, "AUTO");
    EXPECT_TRUE(c.imagingMoveFocus("VideoSource_1", 0.5).ok());
    EXPECT_EQ(http.requests.back().url, "http://192.168.1.64/onvif/Imaging");
}

TEST(ClientPtz, MissingServiceIsUnsupportedWithoutNetwork) {
    FakeHttp http;
    OnvifClient c(http, baseOptions(0));
    auto r = c.ptzStop("p");
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, ErrorCode::Unsupported);
    EXPECT_TRUE(http.requests.empty());
    EXPECT_EQ(c.imagingStop("v").error().code, ErrorCode::Unsupported);
}

TEST(ClientPtz, FaultsFromCameraAreReported) {
    FakeHttp http;
    http.replies["GotoPreset"] = {500, fx::kFault12InvalidArg};
    OnvifClient c(http, baseOptions(0));
    ServiceEndpoints e;
    e.ptz = "http://h/ptz";
    c.setEndpoints(e);
    auto r = c.ptzGotoPreset("p", "99");
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, ErrorCode::SoapFault);
    EXPECT_EQ(r.error().faultCode, "ter:NoProfile");
    EXPECT_EQ(r.error().httpStatus, 500);
}
