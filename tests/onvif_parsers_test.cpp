#include <gtest/gtest.h>

#include <random>
#include <string>

#include "onvif/parsers.hpp"
#include "onvif_fixtures.hpp"

using namespace hb::onvif;

TEST(Parsers, HikvisionDeviceInfo) {
    auto r = parseDeviceInformation(fx::kHikDeviceInfo);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value().manufacturer, "HIKVISION");
    EXPECT_EQ(r.value().model, "DS-2DE4425IW-DE");
    EXPECT_EQ(r.value().firmwareVersion, "V5.6.10 build 200415");
    EXPECT_EQ(r.value().hardwareId, "88");
}

TEST(Parsers, SystemDateAndTimeUsesUtcNotLocal) {
    auto r = parseSystemDateAndTime(fx::kHikDateTime);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value().utcSeconds, fx::kHikDateTimeUnix);
    EXPECT_EQ(r.value().dateTimeType, "NTP");
}

TEST(Parsers, SystemDateAndTimeMissingUtc) {
    const std::string x =
        "<e:Envelope xmlns:e=\"http://www.w3.org/2003/05/soap-envelope\"><e:Body><GetSystemDateAndTimeResponse><SystemDateAndTime>"
        "<DateTimeType>Manual</DateTimeType></SystemDateAndTime></GetSystemDateAndTimeResponse></e:Body></e:Envelope>";
    auto r = parseSystemDateAndTime(x);
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, ErrorCode::MissingField);
}

TEST(Parsers, CapabilitiesHikvisionAndAxis) {
    auto h = parseCapabilities(fx::kHikCapabilities);
    ASSERT_TRUE(h.ok());
    EXPECT_EQ(h.value().media, "http://192.168.1.64/onvif/Media");
    EXPECT_EQ(h.value().ptz, "http://192.168.1.64/onvif/PTZ");
    EXPECT_EQ(h.value().imaging, "http://192.168.1.64/onvif/Imaging");
    auto a = parseCapabilities(fx::kAxisCapabilities);
    ASSERT_TRUE(a.ok());
    EXPECT_EQ(a.value().media, "http://192.168.0.90/onvif/services");
    EXPECT_TRUE(a.value().events.empty());
}

TEST(Parsers, ServicesDahuaIgnoresUnknownNamespaces) {
    auto r = parseServices(fx::kDahuaServices);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value().device, "http://192.168.1.108/onvif/device_service");
    EXPECT_EQ(r.value().media, "http://192.168.1.108/onvif/media_service");
    EXPECT_EQ(r.value().ptz, "http://192.168.1.108/onvif/ptz_service");
    EXPECT_EQ(r.value().imaging, "http://192.168.1.108/onvif/imaging_service");
}

TEST(Parsers, ProfilesHikvision) {
    auto r = parseProfiles(fx::kHikProfiles);
    ASSERT_TRUE(r.ok());
    ASSERT_EQ(r.value().size(), 2u);
    const auto& p = r.value()[0];
    EXPECT_EQ(p.token, "Profile_1");
    EXPECT_TRUE(p.fixed);
    EXPECT_EQ(p.videoSourceToken, "VideoSource_1");
    ASSERT_TRUE(p.video.has_value());
    EXPECT_EQ(p.video->encoding, "H264");
    EXPECT_EQ(p.video->width, 1920);
    EXPECT_EQ(p.video->height, 1080);
    EXPECT_DOUBLE_EQ(p.video->frameRate, 25);
    EXPECT_EQ(p.video->bitrateKbps, 4096);
    EXPECT_EQ(p.video->gopLength, 50);
    EXPECT_TRUE(p.hasPtz());
}

TEST(Parsers, ProfilesDefaultNamespaceAndHevcAlias) {
    auto r = parseProfiles(fx::kGenericProfilesDefaultNs);
    ASSERT_TRUE(r.ok());
    ASSERT_EQ(r.value().size(), 1u);
    EXPECT_EQ(r.value()[0].video->encoding, "H265");
    EXPECT_EQ(r.value()[0].video->height, 720);
    EXPECT_FALSE(r.value()[0].hasPtz());
}

TEST(Parsers, ProfilesSparseSkipsTokenless) {
    auto r = parseProfiles(fx::kProfilesSparse);
    ASSERT_TRUE(r.ok());
    ASSERT_EQ(r.value().size(), 1u);
    EXPECT_EQ(r.value()[0].token, "a");
    EXPECT_FALSE(r.value()[0].video.has_value());
}

TEST(Parsers, StreamUriUnescapesAmpersands) {
    auto r = parseStreamUri(fx::kDahuaStreamUri);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value().uri, "rtsp://192.168.1.108:554/cam/realmonitor?channel=1&subtype=0&unicast=true&proto=Onvif");
    EXPECT_FALSE(r.value().invalidAfterConnect);
    EXPECT_EQ(r.value().timeout, "PT0S");
}

TEST(Parsers, PresetsSkipUnusableAndFillNames) {
    auto r = parsePresets(fx::kPresets);
    ASSERT_TRUE(r.ok());
    ASSERT_EQ(r.value().size(), 3u);
    EXPECT_EQ(r.value()[0].name, "Home");
    EXPECT_EQ(r.value()[1].token, "2");
    EXPECT_EQ(r.value()[2].name, "3");  // no Name: token used
    auto e = parsePresets(fx::kPresetsEmpty);
    ASSERT_TRUE(e.ok());
    EXPECT_TRUE(e.value().empty());
}

TEST(Parsers, SetPresetStatusImagingAck) {
    EXPECT_EQ(parseSetPresetResponse(fx::kSetPresetResponse).value(), "7");
    auto st = parsePtzStatus(fx::kPtzStatus);
    ASSERT_TRUE(st.ok());
    EXPECT_DOUBLE_EQ(*st.value().pan, -0.25);
    EXPECT_DOUBLE_EQ(*st.value().tilt, 0.5);
    EXPECT_DOUBLE_EQ(*st.value().zoom, 0.1);
    EXPECT_EQ(st.value().panTiltMove, "MOVING");
    EXPECT_EQ(st.value().zoomMove, "IDLE");
    auto im = parseImagingSettings(fx::kImagingSettings);
    ASSERT_TRUE(im.ok());
    EXPECT_DOUBLE_EQ(*im.value().brightness, 50);
    EXPECT_DOUBLE_EQ(*im.value().colorSaturation, 48.5);
    EXPECT_EQ(im.value().focusMode, "AUTO");
    EXPECT_TRUE(parseAck(fx::kPtzStopResponse).ok());
}

TEST(Parsers, StatusWithNoPositionIsEmptyNotError) {
    auto st = parsePtzStatus("<e:Envelope xmlns:e=\"http://www.w3.org/2003/05/soap-envelope\"><e:Body><GetStatusResponse/></e:Body></e:Envelope>");
    ASSERT_TRUE(st.ok());
    EXPECT_FALSE(st.value().pan.has_value());
}

TEST(Faults, NotAuthorizedIsTyped) {
    auto r = parseProfiles(fx::kHikNotAuthorized);
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, ErrorCode::Unauthorized);
    EXPECT_EQ(r.error().faultCode, "ter:NotAuthorized");
    EXPECT_NE(r.error().faultReason.find("not Authorized"), std::string::npos);
    EXPECT_FALSE(parseAck(fx::kHikNotAuthorized).ok());
}

TEST(Faults, Soap11AndNestedSubcode) {
    auto a = parseAck(fx::kGenericFault11);
    ASSERT_FALSE(a.ok());
    EXPECT_EQ(a.error().code, ErrorCode::SoapFault);
    EXPECT_EQ(a.error().faultCode, "soap:Client");
    EXPECT_EQ(a.error().faultReason, "Invalid argument: ProfileToken");
    auto b = parseAck(fx::kFault12InvalidArg);
    ASSERT_FALSE(b.ok());
    EXPECT_EQ(b.error().faultCode, "ter:NoProfile");  // innermost subcode
    EXPECT_EQ(b.error().code, ErrorCode::SoapFault);
    EXPECT_TRUE(parseFaultOnly(fx::kFault12InvalidArg).has_value());
    EXPECT_FALSE(parseFaultOnly(fx::kHikProfiles).has_value());
}

TEST(Robustness, MalformedInputYieldsTypedErrors) {
    for (const char* bad : {"", "   ", "not xml at all", "<a><b></a>", "<?xml version=\"1.0\"?>", "<root/>",
                            "<e:Envelope xmlns:e=\"x\"/>", "<e:Envelope xmlns:e=\"x\"><e:Body/></e:Envelope>"}) {
        EXPECT_FALSE(parseProfiles(bad).ok()) << bad;
        EXPECT_FALSE(parseDeviceInformation(bad).ok()) << bad;
        EXPECT_FALSE(parseSystemDateAndTime(bad).ok()) << bad;
        EXPECT_FALSE(parseCapabilities(bad).ok()) << bad;
        EXPECT_FALSE(parseServices(bad).ok()) << bad;
        EXPECT_FALSE(parseStreamUri(bad).ok()) << bad;
        EXPECT_FALSE(parseAck(bad).ok()) << bad;
    }
    auto r = parseProfiles("<a><b></a>");
    EXPECT_EQ(r.error().code, ErrorCode::MalformedXml);
}

TEST(Robustness, TruncatedAndMutatedFixturesNeverCrash) {
    const std::string docs[] = {fx::kHikProfiles, fx::kDahuaServices, fx::kHikDateTime, fx::kPtzStatus, fx::kHikNotAuthorized, fx::kPresets};
    std::mt19937 rng(12345);
    for (const auto& d : docs) {
        for (std::size_t cut = 0; cut < d.size(); cut += 7) {  // every truncation point (stride 7)
            const std::string t = d.substr(0, cut);
            (void)parseProfiles(t); (void)parseServices(t); (void)parseSystemDateAndTime(t);
            (void)parsePtzStatus(t); (void)parsePresets(t); (void)parseAck(t); (void)parseCapabilities(t);
        }
        for (int i = 0; i < 200; ++i) {  // random byte corruption
            std::string m = d;
            for (int k = 0; k < 5; ++k) m[rng() % m.size()] = static_cast<char>(rng() & 0xFF);
            (void)parseProfiles(m); (void)parseServices(m); (void)parseSystemDateAndTime(m);
            (void)parsePtzStatus(m); (void)parseStreamUri(m); (void)parseDeviceInformation(m);
        }
    }
    SUCCEED();
}

TEST(Robustness, ExtremeNumbersAndHostileNesting) {
    const std::string badNumbers =
        "<e:Envelope xmlns:e=\"http://www.w3.org/2003/05/soap-envelope\"><e:Body><r><Profiles token=\"t\"><VideoEncoderConfiguration token=\"v\">"
        "<Resolution><Width>99999999999999999999</Width><Height>-5x</Height></Resolution>"
        "<RateControl><FrameRateLimit>abc</FrameRateLimit></RateControl></VideoEncoderConfiguration></Profiles></r></e:Body></e:Envelope>";
    auto bn = parseProfiles(badNumbers);  // unparsable numbers degrade to 0, never crash
    ASSERT_TRUE(bn.ok());
    EXPECT_EQ(bn.value()[0].video->width, 0);
    EXPECT_EQ(bn.value()[0].video->height, 0);
    EXPECT_DOUBLE_EQ(bn.value()[0].video->frameRate, 0);

    std::string deep = "<e:Envelope xmlns:e=\"http://www.w3.org/2003/05/soap-envelope\"><e:Body><r>";
    for (int i = 0; i < 50000; ++i) deep += "<x>";
    for (int i = 0; i < 50000; ++i) deep += "</x>";
    deep += "</r></e:Body></e:Envelope>";
    EXPECT_FALSE(parseSystemDateAndTime(deep).ok());  // must return an error, not overflow the stack
    (void)parsePtzStatus(deep);
}
