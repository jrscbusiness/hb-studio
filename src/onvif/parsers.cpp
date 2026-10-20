#include "onvif/parsers.hpp"

#include <algorithm>
#include <cctype>
#include <exception>

#include "onvif/soap.hpp"
#include "onvif/time_util.hpp"
#include "onvif/xml_util.hpp"

namespace hb::onvif {
namespace {

using namespace xml;

// Runs `fn` and converts any exception (e.g. bad_alloc) into an Error: nothing escapes.
template <class T, class F>
Result<T> guarded(std::string_view xmlText, F&& fn) {
    try {
        auto r = openResponse(xmlText);
        if (!r.ok()) return r.error();
        return fn(*r.value());
    } catch (const std::exception& e) {
        return makeError(ErrorCode::MalformedXml, std::string("parser exception: ") + e.what());
    } catch (...) {
        return makeError(ErrorCode::MalformedXml, "parser exception");
    }
}

std::string upper(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::string normaliseEncoding(const std::string& raw) {
    std::string u = upper(raw);
    u.erase(std::remove_if(u.begin(), u.end(), [](char c) { return c == '.' || c == '-' || c == ' ' || c == '_'; }), u.end());
    if (u == "H264" || u == "AVC") return "H264";
    if (u == "H265" || u == "HEVC") return "H265";
    if (u == "JPEG" || u == "MJPEG") return "JPEG";
    if (u == "MPEG4") return "MPEG4";
    return raw;
}

std::optional<double> optDouble(pugi::xml_node n) {
    double v;
    if (n && parseDouble(n.text().as_string(""), v)) return v;
    return std::nullopt;
}

std::optional<double> optAttr(pugi::xml_node n, std::string_view name) {
    double v;
    if (n && parseDouble(attr(n, name), v)) return v;
    return std::nullopt;
}

std::string serviceKey(std::string_view nsUri) {
    // Tolerate trailing slash / case differences.
    std::string s(nsUri);
    while (!s.empty() && s.back() == '/') s.pop_back();
    return s;
}

}  // namespace

Result<DeviceInformation> parseDeviceInformation(std::string_view x) {
    return guarded<DeviceInformation>(x, [](Response& r) -> Result<DeviceInformation> {
        DeviceInformation d;
        d.manufacturer = childText(r.payload, "Manufacturer");
        d.model = childText(r.payload, "Model");
        d.firmwareVersion = childText(r.payload, "FirmwareVersion");
        d.serialNumber = childText(r.payload, "SerialNumber");
        d.hardwareId = childText(r.payload, "HardwareId");
        if (d.manufacturer.empty() && d.model.empty() && d.serialNumber.empty())
            return makeError(ErrorCode::MissingField, "GetDeviceInformation response has no identity fields");
        return d;
    });
}

Result<SystemDateTime> parseSystemDateAndTime(std::string_view x) {
    return guarded<SystemDateTime>(x, [](Response& r) -> Result<SystemDateTime> {
        auto sdt = descendant(r.payload, "SystemDateAndTime");
        if (!sdt) sdt = r.payload;
        SystemDateTime out;
        out.dateTimeType = childText(sdt, "DateTimeType");
        auto utc = child(sdt, "UTCDateTime");
        if (!utc) return makeError(ErrorCode::MissingField, "SystemDateAndTime has no UTCDateTime");
        int Y, M, D, h, m, s;
        auto t = child(utc, "Time");
        auto d = child(utc, "Date");
        if (!t || !d || !parseInt(childText(d, "Year"), Y) || !parseInt(childText(d, "Month"), M) ||
            !parseInt(childText(d, "Day"), D) || !parseInt(childText(t, "Hour"), h) || !parseInt(childText(t, "Minute"), m) ||
            !parseInt(childText(t, "Second"), s))
            return makeError(ErrorCode::MissingField, "UTCDateTime is incomplete");
        auto secs = civilToUnix(Y, M, D, h, m, s);
        if (!secs) return makeError(ErrorCode::MissingField, "UTCDateTime out of range");
        out.utcSeconds = *secs;
        return out;
    });
}

Result<ServiceEndpoints> parseCapabilities(std::string_view x) {
    return guarded<ServiceEndpoints>(x, [](Response& r) -> Result<ServiceEndpoints> {
        auto caps = descendant(r.payload, "Capabilities");
        if (!caps) caps = r.payload;
        ServiceEndpoints e;
        e.device = text(path(caps, "Device/XAddr"));
        e.media = text(path(caps, "Media/XAddr"));
        e.ptz = text(path(caps, "PTZ/XAddr"));
        e.imaging = text(path(caps, "Imaging/XAddr"));
        e.events = text(path(caps, "Events/XAddr"));
        if (e.device.empty() && e.media.empty() && e.ptz.empty() && e.imaging.empty())
            return makeError(ErrorCode::MissingField, "GetCapabilities returned no XAddr");
        return e;
    });
}

Result<ServiceEndpoints> parseServices(std::string_view x) {
    return guarded<ServiceEndpoints>(x, [](Response& r) -> Result<ServiceEndpoints> {
        ServiceEndpoints e;
        std::string media2;
        bool any = false;
        for (auto s = r.payload.first_child(); s; s = s.next_sibling()) {
            if (s.type() != pugi::node_element || localName(s.name()) != "Service") continue;
            const std::string nsUri = serviceKey(childText(s, "Namespace"));
            const std::string xaddr = childText(s, "XAddr");
            if (xaddr.empty()) continue;
            any = true;
            if (nsUri == ns::kDevice) e.device = xaddr;
            else if (nsUri == ns::kMedia) e.media = xaddr;
            else if (nsUri == ns::kMedia2) media2 = xaddr;
            else if (nsUri == ns::kPtz) e.ptz = xaddr;
            else if (nsUri == ns::kImaging) e.imaging = xaddr;
            else if (nsUri == ns::kEvents) e.events = xaddr;
        }
        // We only speak Media ver10; Media2 is not used as a substitute (different messages).
        (void)media2;
        if (!any) return makeError(ErrorCode::MissingField, "GetServices returned no Service with XAddr");
        return e;
    });
}

Result<std::vector<MediaProfile>> parseProfiles(std::string_view x) {
    return guarded<std::vector<MediaProfile>>(x, [](Response& r) -> Result<std::vector<MediaProfile>> {
        std::vector<MediaProfile> out;
        for (auto p = r.payload.first_child(); p; p = p.next_sibling()) {
            if (p.type() != pugi::node_element || localName(p.name()) != "Profiles") continue;
            MediaProfile mp;
            mp.token = attr(p, "token");
            if (mp.token.empty()) continue;  // unusable without a token
            mp.name = childText(p, "Name");
            mp.fixed = attr(p, "fixed") == "true";
            if (auto vs = child(p, "VideoSourceConfiguration")) mp.videoSourceToken = childText(vs, "SourceToken");
            if (auto ve = child(p, "VideoEncoderConfiguration")) {
                VideoEncoderConfig v;
                v.token = attr(ve, "token");
                v.encoding = normaliseEncoding(childText(ve, "Encoding"));
                parseInt(childText(path(ve, "Resolution"), "Width"), v.width);
                parseInt(childText(path(ve, "Resolution"), "Height"), v.height);
                if (auto rc = child(ve, "RateControl")) {
                    parseDouble(childText(rc, "FrameRateLimit"), v.frameRate);
                    parseInt(childText(rc, "BitrateLimit"), v.bitrateKbps);
                }
                if (auto h = child(ve, "H264")) parseInt(childText(h, "GovLength"), v.gopLength);
                else if (auto h5 = child(ve, "H265")) parseInt(childText(h5, "GovLength"), v.gopLength);
                mp.video = v;
            }
            if (auto pc = child(p, "PTZConfiguration")) {
                mp.ptzConfigToken = attr(pc, "token");
                if (mp.ptzConfigToken.empty()) mp.ptzConfigToken = childText(pc, "Name");
                if (mp.ptzConfigToken.empty()) mp.ptzConfigToken = "ptz";
            }
            out.push_back(std::move(mp));
        }
        if (out.empty()) return makeError(ErrorCode::MissingField, "GetProfiles returned no usable profile");
        return out;
    });
}

Result<StreamUri> parseStreamUri(std::string_view x) {
    return guarded<StreamUri>(x, [](Response& r) -> Result<StreamUri> {
        auto mu = descendant(r.payload, "MediaUri");
        if (!mu) mu = r.payload;
        StreamUri s;
        s.uri = childText(mu, "Uri");
        if (s.uri.empty()) return makeError(ErrorCode::MissingField, "GetStreamUri response has no Uri");
        s.invalidAfterConnect = childText(mu, "InvalidAfterConnect") == "true";
        s.invalidAfterReboot = childText(mu, "InvalidAfterReboot") == "true";
        s.timeout = childText(mu, "Timeout");
        return s;
    });
}

Result<std::vector<PtzPreset>> parsePresets(std::string_view x) {
    return guarded<std::vector<PtzPreset>>(x, [](Response& r) -> Result<std::vector<PtzPreset>> {
        std::vector<PtzPreset> out;  // an empty list is valid (camera has no presets)
        for (auto p = r.payload.first_child(); p; p = p.next_sibling()) {
            if (p.type() != pugi::node_element || localName(p.name()) != "Preset") continue;
            PtzPreset pr;
            pr.token = attr(p, "token");
            pr.name = childText(p, "Name");
            if (pr.token.empty()) continue;
            if (pr.name.empty()) pr.name = pr.token;
            out.push_back(std::move(pr));
        }
        return out;
    });
}

Result<std::string> parseSetPresetResponse(std::string_view x) {
    return guarded<std::string>(x, [](Response& r) -> Result<std::string> {
        auto t = childText(r.payload, "PresetToken");
        if (t.empty()) return makeError(ErrorCode::MissingField, "SetPreset response has no PresetToken");
        return t;
    });
}

Result<PtzStatus> parsePtzStatus(std::string_view x) {
    return guarded<PtzStatus>(x, [](Response& r) -> Result<PtzStatus> {
        auto st = descendant(r.payload, "PTZStatus");
        if (!st) st = r.payload;
        PtzStatus s;
        if (auto pos = child(st, "Position")) {
            auto pt = child(pos, "PanTilt");
            s.pan = optAttr(pt, "x");
            s.tilt = optAttr(pt, "y");
            s.zoom = optAttr(child(pos, "Zoom"), "x");
        }
        if (auto mv = child(st, "MoveStatus")) {
            s.panTiltMove = childText(mv, "PanTilt");
            s.zoomMove = childText(mv, "Zoom");
        }
        return s;
    });
}

Result<ImagingSettings> parseImagingSettings(std::string_view x) {
    return guarded<ImagingSettings>(x, [](Response& r) -> Result<ImagingSettings> {
        auto is = descendant(r.payload, "ImagingSettings");
        if (!is) is = r.payload;
        ImagingSettings s;
        s.brightness = optDouble(child(is, "Brightness"));
        s.contrast = optDouble(child(is, "Contrast"));
        s.colorSaturation = optDouble(child(is, "ColorSaturation"));
        s.sharpness = optDouble(child(is, "Sharpness"));
        s.focusMode = childText(path(is, "Focus"), "AutoFocusMode");
        s.irCutFilter = childText(is, "IrCutFilter");
        return s;
    });
}

Result<Unit> parseAck(std::string_view x) {
    return guarded<Unit>(x, [](Response&) -> Result<Unit> { return Unit{}; });
}

std::optional<Error> parseFaultOnly(std::string_view x) {
    try {
        if (x.empty()) return std::nullopt;
        pugi::xml_document doc;
        if (!doc.load_buffer(x.data(), x.size())) return std::nullopt;
        auto env = child(doc, "Envelope");
        auto body = child(env, "Body");
        Error e;
        if (body && extractFault(body, e)) return e;
    } catch (...) {
    }
    return std::nullopt;
}

}  // namespace hb::onvif
