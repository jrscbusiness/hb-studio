#pragma once
// SOAP response fixtures. Inspired by the typical structure of responses from Hikvision, Dahua and
// Axis firmware plus a generic default-namespace style. They are hand-written reconstructions,
// NOT captures from real cameras; addresses, serials and tokens are fictitious.
namespace fx {

#define HB_ENV_OPEN(P)                                                                                       \
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?><" P ":Envelope xmlns:" P "=\"http://www.w3.org/2003/05/soap-envelope\" " \
    "xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\" xmlns:trt=\"http://www.onvif.org/ver10/media/wsdl\" "         \
    "xmlns:tptz=\"http://www.onvif.org/ver20/ptz/wsdl\" xmlns:timg=\"http://www.onvif.org/ver20/imaging/wsdl\" "         \
    "xmlns:tt=\"http://www.onvif.org/ver10/schema\" xmlns:ter=\"http://www.onvif.org/ver10/error\"><" P ":Body>"
#define HB_ENV_CLOSE(P) "</" P ":Body></" P ":Envelope>"

// ---------------- Hikvision style (SOAP-ENV prefix) ----------------
inline constexpr const char* kHikDeviceInfo = HB_ENV_OPEN("SOAP-ENV")
    "<tds:GetDeviceInformationResponse><tds:Manufacturer>HIKVISION</tds:Manufacturer><tds:Model>DS-2DE4425IW-DE</tds:Model>"
    "<tds:FirmwareVersion>V5.6.10 build 200415</tds:FirmwareVersion><tds:SerialNumber>DS-2DE4425IW-DE20200101AAWRE12345678</tds:SerialNumber>"
    "<tds:HardwareId>88</tds:HardwareId></tds:GetDeviceInformationResponse>" HB_ENV_CLOSE("SOAP-ENV");

inline constexpr const char* kHikDateTime = HB_ENV_OPEN("SOAP-ENV")
    "<tds:GetSystemDateAndTimeResponse><tds:SystemDateAndTime><tt:DateTimeType>NTP</tt:DateTimeType><tt:DaylightSavings>false</tt:DaylightSavings>"
    "<tt:TimeZone><tt:TZ>CST+3:00:00</tt:TZ></tt:TimeZone>"
    "<tt:UTCDateTime><tt:Time><tt:Hour>13</tt:Hour><tt:Minute>5</tt:Minute><tt:Second>9</tt:Second></tt:Time>"
    "<tt:Date><tt:Year>2024</tt:Year><tt:Month>3</tt:Month><tt:Day>7</tt:Day></tt:Date></tt:UTCDateTime>"
    "<tt:LocalDateTime><tt:Time><tt:Hour>10</tt:Hour><tt:Minute>5</tt:Minute><tt:Second>9</tt:Second></tt:Time>"
    "<tt:Date><tt:Year>2024</tt:Year><tt:Month>3</tt:Month><tt:Day>7</tt:Day></tt:Date></tt:LocalDateTime>"
    "</tds:SystemDateAndTime></tds:GetSystemDateAndTimeResponse>" HB_ENV_CLOSE("SOAP-ENV");
// 2024-03-07T13:05:09Z
inline constexpr long long kHikDateTimeUnix = 1709816709LL;

inline constexpr const char* kHikCapabilities = HB_ENV_OPEN("SOAP-ENV")
    "<tds:GetCapabilitiesResponse><tds:Capabilities>"
    "<tt:Analytics><tt:XAddr>http://192.168.1.64/onvif/Analytics</tt:XAddr><tt:RuleSupport>true</tt:RuleSupport></tt:Analytics>"
    "<tt:Device><tt:XAddr>http://192.168.1.64/onvif/device_service</tt:XAddr></tt:Device>"
    "<tt:Events><tt:XAddr>http://192.168.1.64/onvif/Events</tt:XAddr></tt:Events>"
    "<tt:Imaging><tt:XAddr>http://192.168.1.64/onvif/Imaging</tt:XAddr></tt:Imaging>"
    "<tt:Media><tt:XAddr>http://192.168.1.64/onvif/Media</tt:XAddr>"
    "<tt:StreamingCapabilities><tt:RTPMulticast>false</tt:RTPMulticast><tt:RTP_TCP>true</tt:RTP_TCP><tt:RTP_RTSP_TCP>true</tt:RTP_RTSP_TCP></tt:StreamingCapabilities></tt:Media>"
    "<tt:PTZ><tt:XAddr>http://192.168.1.64/onvif/PTZ</tt:XAddr></tt:PTZ>"
    "</tds:Capabilities></tds:GetCapabilitiesResponse>" HB_ENV_CLOSE("SOAP-ENV");

inline constexpr const char* kHikProfiles = HB_ENV_OPEN("SOAP-ENV")
    "<trt:GetProfilesResponse>"
    "<trt:Profiles token=\"Profile_1\" fixed=\"true\"><tt:Name>mainStream</tt:Name>"
    "<tt:VideoSourceConfiguration token=\"VideoSourceToken\"><tt:Name>VideoSourceConfig</tt:Name><tt:UseCount>2</tt:UseCount>"
    "<tt:SourceToken>VideoSource_1</tt:SourceToken><tt:Bounds x=\"0\" y=\"0\" width=\"1920\" height=\"1080\"/></tt:VideoSourceConfiguration>"
    "<tt:VideoEncoderConfiguration token=\"VideoEncoderToken_1\"><tt:Name>VideoEncoder_1</tt:Name><tt:UseCount>1</tt:UseCount>"
    "<tt:Encoding>H264</tt:Encoding><tt:Resolution><tt:Width>1920</tt:Width><tt:Height>1080</tt:Height></tt:Resolution>"
    "<tt:Quality>4</tt:Quality><tt:RateControl><tt:FrameRateLimit>25</tt:FrameRateLimit><tt:EncodingInterval>1</tt:EncodingInterval><tt:BitrateLimit>4096</tt:BitrateLimit></tt:RateControl>"
    "<tt:H264><tt:GovLength>50</tt:GovLength><tt:H264Profile>Main</tt:H264Profile></tt:H264></tt:VideoEncoderConfiguration>"
    "<tt:PTZConfiguration token=\"PTZToken\"><tt:Name>PTZConfig</tt:Name><tt:UseCount>2</tt:UseCount><tt:NodeToken>PTZNodeToken</tt:NodeToken></tt:PTZConfiguration>"
    "</trt:Profiles>"
    "<trt:Profiles token=\"Profile_2\" fixed=\"true\"><tt:Name>subStream</tt:Name>"
    "<tt:VideoSourceConfiguration token=\"VideoSourceToken\"><tt:Name>VideoSourceConfig</tt:Name><tt:UseCount>2</tt:UseCount><tt:SourceToken>VideoSource_1</tt:SourceToken></tt:VideoSourceConfiguration>"
    "<tt:VideoEncoderConfiguration token=\"VideoEncoderToken_2\"><tt:Name>VideoEncoder_2</tt:Name><tt:Encoding>H264</tt:Encoding>"
    "<tt:Resolution><tt:Width>640</tt:Width><tt:Height>360</tt:Height></tt:Resolution>"
    "<tt:RateControl><tt:FrameRateLimit>15</tt:FrameRateLimit><tt:BitrateLimit>512</tt:BitrateLimit></tt:RateControl>"
    "<tt:H264><tt:GovLength>30</tt:GovLength></tt:H264></tt:VideoEncoderConfiguration>"
    "<tt:PTZConfiguration token=\"PTZToken\"><tt:Name>PTZConfig</tt:Name></tt:PTZConfiguration>"
    "</trt:Profiles></trt:GetProfilesResponse>" HB_ENV_CLOSE("SOAP-ENV");

inline constexpr const char* kHikStreamUriMain = HB_ENV_OPEN("SOAP-ENV")
    "<trt:GetStreamUriResponse><trt:MediaUri><tt:Uri>rtsp://192.168.1.64:554/Streaming/Channels/101?transportmode=unicast&amp;profile=Profile_1</tt:Uri>"
    "<tt:InvalidAfterConnect>false</tt:InvalidAfterConnect><tt:InvalidAfterReboot>false</tt:InvalidAfterReboot><tt:Timeout>PT60S</tt:Timeout></trt:MediaUri>"
    "</trt:GetStreamUriResponse>" HB_ENV_CLOSE("SOAP-ENV");

inline constexpr const char* kHikStreamUriSub = HB_ENV_OPEN("SOAP-ENV")
    "<trt:GetStreamUriResponse><trt:MediaUri><tt:Uri>rtsp://192.168.1.64:554/Streaming/Channels/102?transportmode=unicast&amp;profile=Profile_2</tt:Uri>"
    "<tt:InvalidAfterConnect>false</tt:InvalidAfterConnect><tt:InvalidAfterReboot>false</tt:InvalidAfterReboot><tt:Timeout>PT60S</tt:Timeout></trt:MediaUri>"
    "</trt:GetStreamUriResponse>" HB_ENV_CLOSE("SOAP-ENV");

inline constexpr const char* kHikNotAuthorized = HB_ENV_OPEN("SOAP-ENV")
    "<SOAP-ENV:Fault><SOAP-ENV:Code><SOAP-ENV:Value>SOAP-ENV:Sender</SOAP-ENV:Value><SOAP-ENV:Subcode><SOAP-ENV:Value>ter:NotAuthorized</SOAP-ENV:Value></SOAP-ENV:Subcode></SOAP-ENV:Code>"
    "<SOAP-ENV:Reason><SOAP-ENV:Text xml:lang=\"en\">Sender not Authorized</SOAP-ENV:Text></SOAP-ENV:Reason></SOAP-ENV:Fault>" HB_ENV_CLOSE("SOAP-ENV");

// ---------------- Dahua style (GetServices, 'env' prefix) ----------------
inline constexpr const char* kDahuaServices = HB_ENV_OPEN("env")
    "<tds:GetServicesResponse>"
    "<tds:Service><tds:Namespace>http://www.onvif.org/ver10/device/wsdl</tds:Namespace><tds:XAddr>http://192.168.1.108/onvif/device_service</tds:XAddr><tds:Version><tt:Major>2</tt:Major><tt:Minor>5</tt:Minor></tds:Version></tds:Service>"
    "<tds:Service><tds:Namespace>http://www.onvif.org/ver10/media/wsdl</tds:Namespace><tds:XAddr>http://192.168.1.108/onvif/media_service</tds:XAddr><tds:Version><tt:Major>2</tt:Major><tt:Minor>5</tt:Minor></tds:Version></tds:Service>"
    "<tds:Service><tds:Namespace>http://www.onvif.org/ver20/ptz/wsdl</tds:Namespace><tds:XAddr>http://192.168.1.108/onvif/ptz_service</tds:XAddr><tds:Version><tt:Major>2</tt:Major><tt:Minor>5</tt:Minor></tds:Version></tds:Service>"
    "<tds:Service><tds:Namespace>http://www.onvif.org/ver20/imaging/wsdl</tds:Namespace><tds:XAddr>http://192.168.1.108/onvif/imaging_service</tds:XAddr></tds:Service>"
    "<tds:Service><tds:Namespace>http://www.onvif.org/ver10/events/wsdl</tds:Namespace><tds:XAddr>http://192.168.1.108/onvif/events_service</tds:XAddr></tds:Service>"
    "<tds:Service><tds:Namespace>http://www.onvif.org/ver20/analytics/wsdl</tds:Namespace><tds:XAddr>http://192.168.1.108/onvif/analytics_service</tds:XAddr></tds:Service>"
    "</tds:GetServicesResponse>" HB_ENV_CLOSE("env");

inline constexpr const char* kDahuaProfiles = HB_ENV_OPEN("env")
    "<trt:GetProfilesResponse>"
    "<trt:Profiles fixed=\"true\" token=\"MediaProfile00000\"><tt:Name>MediaProfile_Channel1_MainStream</tt:Name>"
    "<tt:VideoSourceConfiguration token=\"VideoSourceConfiguration\"><tt:Name>VideoSourceConfiguration</tt:Name><tt:SourceToken>raw_sensor</tt:SourceToken></tt:VideoSourceConfiguration>"
    "<tt:VideoEncoderConfiguration token=\"VideoEncoderConfiguration000\"><tt:Name>VideoEncoder_1</tt:Name><tt:Encoding>H265</tt:Encoding>"
    "<tt:Resolution><tt:Width>2688</tt:Width><tt:Height>1520</tt:Height></tt:Resolution><tt:Quality>5</tt:Quality>"
    "<tt:RateControl><tt:FrameRateLimit>25</tt:FrameRateLimit><tt:EncodingInterval>1</tt:EncodingInterval><tt:BitrateLimit>4096</tt:BitrateLimit></tt:RateControl>"
    "<tt:H264><tt:GovLength>50</tt:GovLength><tt:H264Profile>Main</tt:H264Profile></tt:H264></tt:VideoEncoderConfiguration>"
    "</trt:Profiles>"
    "<trt:Profiles fixed=\"true\" token=\"MediaProfile00001\"><tt:Name>MediaProfile_Channel1_SubStream1</tt:Name>"
    "<tt:VideoEncoderConfiguration token=\"VideoEncoderConfiguration001\"><tt:Name>VideoEncoder_2</tt:Name><tt:Encoding>H264</tt:Encoding>"
    "<tt:Resolution><tt:Width>704</tt:Width><tt:Height>576</tt:Height></tt:Resolution>"
    "<tt:RateControl><tt:FrameRateLimit>25</tt:FrameRateLimit><tt:BitrateLimit>1024</tt:BitrateLimit></tt:RateControl></tt:VideoEncoderConfiguration>"
    "</trt:Profiles>"
    "<trt:Profiles fixed=\"true\" token=\"MediaProfile00002\"><tt:Name>MediaProfile_Channel1_SubStream2</tt:Name>"
    "<tt:VideoEncoderConfiguration token=\"VideoEncoderConfiguration002\"><tt:Name>VideoEncoder_3</tt:Name><tt:Encoding>JPEG</tt:Encoding>"
    "<tt:Resolution><tt:Width>352</tt:Width><tt:Height>288</tt:Height></tt:Resolution>"
    "<tt:RateControl><tt:FrameRateLimit>5</tt:FrameRateLimit><tt:BitrateLimit>256</tt:BitrateLimit></tt:RateControl></tt:VideoEncoderConfiguration>"
    "</trt:Profiles></trt:GetProfilesResponse>" HB_ENV_CLOSE("env");

inline constexpr const char* kDahuaStreamUri = HB_ENV_OPEN("env")
    "<trt:GetStreamUriResponse><trt:MediaUri><tt:Uri>rtsp://192.168.1.108:554/cam/realmonitor?channel=1&amp;subtype=0&amp;unicast=true&amp;proto=Onvif</tt:Uri>"
    "<tt:InvalidAfterConnect>false</tt:InvalidAfterConnect><tt:InvalidAfterReboot>false</tt:InvalidAfterReboot><tt:Timeout>PT0S</tt:Timeout></trt:MediaUri>"
    "</trt:GetStreamUriResponse>" HB_ENV_CLOSE("env");

// ---------------- Axis style ----------------
inline constexpr const char* kAxisCapabilities = HB_ENV_OPEN("SOAP-ENV")
    "<tds:GetCapabilitiesResponse><tds:Capabilities>"
    "<tt:Device><tt:XAddr>http://192.168.0.90/onvif/device_service</tt:XAddr><tt:System><tt:DiscoveryResolve>false</tt:DiscoveryResolve></tt:System></tt:Device>"
    "<tt:Imaging><tt:XAddr>http://192.168.0.90/onvif/services</tt:XAddr></tt:Imaging>"
    "<tt:Media><tt:XAddr>http://192.168.0.90/onvif/services</tt:XAddr></tt:Media>"
    "<tt:PTZ><tt:XAddr>http://192.168.0.90/onvif/services</tt:XAddr></tt:PTZ>"
    "</tds:Capabilities></tds:GetCapabilitiesResponse>" HB_ENV_CLOSE("SOAP-ENV");

inline constexpr const char* kAxisProfiles = HB_ENV_OPEN("SOAP-ENV")
    "<trt:GetProfilesResponse>"
    "<trt:Profiles token=\"profile_1_jpeg\" fixed=\"true\"><tt:Name>profile_1 jpeg</tt:Name>"
    "<tt:VideoSourceConfiguration token=\"1\"><tt:Name>VideoSourceConfig</tt:Name><tt:SourceToken>1</tt:SourceToken></tt:VideoSourceConfiguration>"
    "<tt:VideoEncoderConfiguration token=\"jpeg\"><tt:Name>jpeg</tt:Name><tt:Encoding>JPEG</tt:Encoding><tt:Resolution><tt:Width>1920</tt:Width><tt:Height>1080</tt:Height></tt:Resolution>"
    "<tt:RateControl><tt:FrameRateLimit>30</tt:FrameRateLimit><tt:BitrateLimit>0</tt:BitrateLimit></tt:RateControl></tt:VideoEncoderConfiguration>"
    "</trt:Profiles>"
    "<trt:Profiles token=\"profile_2_h264\" fixed=\"true\"><tt:Name>profile_2 h264</tt:Name>"
    "<tt:VideoSourceConfiguration token=\"1\"><tt:Name>VideoSourceConfig</tt:Name><tt:SourceToken>1</tt:SourceToken></tt:VideoSourceConfiguration>"
    "<tt:VideoEncoderConfiguration token=\"h264\"><tt:Name>h264</tt:Name><tt:Encoding>H264</tt:Encoding><tt:Resolution><tt:Width>1920</tt:Width><tt:Height>1080</tt:Height></tt:Resolution>"
    "<tt:RateControl><tt:FrameRateLimit>30</tt:FrameRateLimit><tt:BitrateLimit>8192</tt:BitrateLimit></tt:RateControl><tt:H264><tt:GovLength>32</tt:GovLength></tt:H264></tt:VideoEncoderConfiguration>"
    "<tt:PTZConfiguration token=\"1\"><tt:Name>PTZConfig</tt:Name></tt:PTZConfiguration>"
    "</trt:Profiles>"
    "<trt:Profiles token=\"profile_3_h264_low\" fixed=\"false\"><tt:Name>low</tt:Name>"
    "<tt:VideoEncoderConfiguration token=\"low\"><tt:Name>low</tt:Name><tt:Encoding>H264</tt:Encoding><tt:Resolution><tt:Width>800</tt:Width><tt:Height>450</tt:Height></tt:Resolution>"
    "<tt:RateControl><tt:FrameRateLimit>15</tt:FrameRateLimit><tt:BitrateLimit>1000</tt:BitrateLimit></tt:RateControl></tt:VideoEncoderConfiguration>"
    "</trt:Profiles></trt:GetProfilesResponse>" HB_ENV_CLOSE("SOAP-ENV");

// ---------------- Generic: default namespaces, no prefixes in the payload ----------------
inline constexpr const char* kGenericProfilesDefaultNs =
    "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\"><s:Body>"
    "<GetProfilesResponse xmlns=\"http://www.onvif.org/ver10/media/wsdl\" xmlns:t=\"http://www.onvif.org/ver10/schema\">"
    "<Profiles token=\"p0\"><Name xmlns=\"http://www.onvif.org/ver10/schema\">only</Name>"
    "<VideoEncoderConfiguration xmlns=\"http://www.onvif.org/ver10/schema\" token=\"e0\"><Encoding>HEVC</Encoding>"
    "<Resolution><Width>1280</Width><Height>720</Height></Resolution></VideoEncoderConfiguration></Profiles>"
    "</GetProfilesResponse></s:Body></s:Envelope>";

// Minimal/odd profile: no encoder config at all, plus one without token (must be skipped).
inline constexpr const char* kProfilesSparse =
    "<soapenv:Envelope xmlns:soapenv=\"http://www.w3.org/2003/05/soap-envelope\"><soapenv:Body>"
    "<ns1:GetProfilesResponse xmlns:ns1=\"http://www.onvif.org/ver10/media/wsdl\">"
    "<ns1:Profiles token=\"a\"/><ns1:Profiles><ns2:Name xmlns:ns2=\"http://www.onvif.org/ver10/schema\">no token</ns2:Name></ns1:Profiles>"
    "</ns1:GetProfilesResponse></soapenv:Body></soapenv:Envelope>";

inline constexpr const char* kGenericFault11 =
    "<?xml version=\"1.0\"?><soap:Envelope xmlns:soap=\"http://schemas.xmlsoap.org/soap/envelope/\"><soap:Body>"
    "<soap:Fault><faultcode>soap:Client</faultcode><faultstring>Invalid argument: ProfileToken</faultstring></soap:Fault></soap:Body></soap:Envelope>";

inline constexpr const char* kFault12InvalidArg = HB_ENV_OPEN("s")
    "<s:Fault><s:Code><s:Value>s:Sender</s:Value><s:Subcode><s:Value>ter:InvalidArgVal</s:Value><s:Subcode><s:Value>ter:NoProfile</s:Value></s:Subcode></s:Subcode></s:Code>"
    "<s:Reason><s:Text xml:lang=\"en\">Profile does not exist</s:Text></s:Reason></s:Fault>" HB_ENV_CLOSE("s");

// ---------------- PTZ / Imaging ----------------
inline constexpr const char* kPresets = HB_ENV_OPEN("SOAP-ENV")
    "<tptz:GetPresetsResponse>"
    "<tptz:Preset token=\"1\"><tt:Name>Home</tt:Name><tt:PTZPosition><tt:PanTilt x=\"0\" y=\"0\"/><tt:Zoom x=\"0\"/></tt:PTZPosition></tptz:Preset>"
    "<tptz:Preset token=\"2\"><tt:Name>Altar</tt:Name></tptz:Preset>"
    "<tptz:Preset token=\"3\"/>"
    "<tptz:Preset><tt:Name>no token</tt:Name></tptz:Preset>"
    "</tptz:GetPresetsResponse>" HB_ENV_CLOSE("SOAP-ENV");

inline constexpr const char* kPresetsEmpty = HB_ENV_OPEN("SOAP-ENV") "<tptz:GetPresetsResponse/>" HB_ENV_CLOSE("SOAP-ENV");

inline constexpr const char* kSetPresetResponse = HB_ENV_OPEN("SOAP-ENV")
    "<tptz:SetPresetResponse><tptz:PresetToken>7</tptz:PresetToken></tptz:SetPresetResponse>" HB_ENV_CLOSE("SOAP-ENV");

inline constexpr const char* kPtzStatus = HB_ENV_OPEN("SOAP-ENV")
    "<tptz:GetStatusResponse><tptz:PTZStatus><tt:Position><tt:PanTilt x=\"-0.25\" y=\"0.5\" space=\"http://www.onvif.org/ver10/tptz/PanTiltSpaces/PositionGenericSpace\"/>"
    "<tt:Zoom x=\"0.1\" space=\"http://www.onvif.org/ver10/tptz/ZoomSpaces/PositionGenericSpace\"/></tt:Position>"
    "<tt:MoveStatus><tt:PanTilt>MOVING</tt:PanTilt><tt:Zoom>IDLE</tt:Zoom></tt:MoveStatus><tt:UtcTime>2024-03-07T13:05:09Z</tt:UtcTime></tptz:PTZStatus></tptz:GetStatusResponse>"
    HB_ENV_CLOSE("SOAP-ENV");

inline constexpr const char* kPtzStopResponse = HB_ENV_OPEN("SOAP-ENV") "<tptz:StopResponse/>" HB_ENV_CLOSE("SOAP-ENV");

inline constexpr const char* kImagingSettings = HB_ENV_OPEN("SOAP-ENV")
    "<timg:GetImagingSettingsResponse><timg:ImagingSettings><tt:Brightness>50</tt:Brightness><tt:ColorSaturation>48.5</tt:ColorSaturation>"
    "<tt:Contrast>55</tt:Contrast><tt:IrCutFilter>AUTO</tt:IrCutFilter><tt:Sharpness>50</tt:Sharpness>"
    "<tt:Focus><tt:AutoFocusMode>AUTO</tt:AutoFocusMode><tt:DefaultSpeed>1</tt:DefaultSpeed></tt:Focus></timg:ImagingSettings></timg:GetImagingSettingsResponse>"
    HB_ENV_CLOSE("SOAP-ENV");

}  // namespace fx
