#include "discovery/ws_discovery.hpp"

#include <algorithm>
#include <cctype>
#include <exception>
#include <sstream>

#include <pugixml.hpp>

#include "onvif/soap.hpp"
#include "onvif/url.hpp"
#include "onvif/xml_util.hpp"

namespace hb::discovery {
namespace {

using hb::onvif::ErrorCode;
using hb::onvif::makeError;
namespace x = hb::onvif::xml;

std::string lower(std::string_view s) {
    std::string r(s);
    for (auto& c : r) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return r;
}

std::vector<std::string> splitWs(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream is(s);
    std::string t;
    while (is >> t) out.push_back(t);
    return out;
}

bool isIpv4Literal(const std::string& h) {
    if (h.empty()) return false;
    int dots = 0;
    for (char c : h) {
        if (c == '.') ++dots;
        else if (c < '0' || c > '9') return false;
    }
    return dots == 3;
}

}  // namespace

std::string buildProbe(std::string_view messageId) {
    return "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
           "<e:Envelope xmlns:e=\"http://www.w3.org/2003/05/soap-envelope\" "
           "xmlns:w=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\" "
           "xmlns:d=\"http://schemas.xmlsoap.org/ws/2005/04/discovery\" "
           "xmlns:dn=\"http://www.onvif.org/ver10/network/wsdl\">"
           "<e:Header><w:MessageID>" +
           hb::onvif::xmlEscape(messageId) +
           "</w:MessageID><w:To e:mustUnderstand=\"true\">urn:schemas-xmlsoap-org:ws:2005:04:discovery</w:To>"
           "<w:Action e:mustUnderstand=\"true\">http://schemas.xmlsoap.org/ws/2005/04/discovery/Probe</w:Action></e:Header>"
           "<e:Body><d:Probe><d:Types>dn:NetworkVideoTransmitter</d:Types></d:Probe></e:Body></e:Envelope>";
}

hb::onvif::Result<std::vector<ProbeMatch>> parseProbeMatches(std::string_view xml) {
    try {
        if (xml.empty() || xml.size() > 64 * 1024) return makeError(ErrorCode::MalformedXml, "datagram empty or too large");
        pugi::xml_document doc;
        const auto r = doc.load_buffer(xml.data(), xml.size());
        if (!r) return makeError(ErrorCode::MalformedXml, std::string("XML parse error: ") + r.description());
        auto env = x::child(doc, "Envelope");
        if (!env) return makeError(ErrorCode::MalformedXml, "no Envelope");
        const std::string relates = x::childText(x::child(env, "Header"), "RelatesTo");
        auto pms = x::path(env, "Body/ProbeMatches");
        if (!pms) return makeError(ErrorCode::MissingField, "not a ProbeMatches message");
        std::vector<ProbeMatch> out;
        for (auto m = pms.first_child(); m; m = m.next_sibling()) {
            if (m.type() != pugi::node_element || x::localName(m.name()) != "ProbeMatch") continue;
            ProbeMatch pm;
            pm.relatesTo = relates;
            pm.endpointAddress = x::text(x::path(m, "EndpointReference/Address"));
            pm.types = splitWs(x::childText(m, "Types"));
            pm.scopes = splitWs(x::childText(m, "Scopes"));
            pm.xaddrs = splitWs(x::childText(m, "XAddrs"));
            x::parseInt(x::childText(m, "MetadataVersion"), pm.metadataVersion);
            if (pm.xaddrs.empty() && pm.endpointAddress.empty()) continue;  // useless match
            out.push_back(std::move(pm));
        }
        if (out.empty()) return makeError(ErrorCode::MissingField, "ProbeMatches without usable ProbeMatch");
        return out;
    } catch (const std::exception& e) {
        return makeError(ErrorCode::MalformedXml, std::string("parser exception: ") + e.what());
    } catch (...) {
        return makeError(ErrorCode::MalformedXml, "parser exception");
    }
}

std::string percentDecode(std::string_view s) {
    std::string out;
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && hex(s[i + 1]) >= 0 && hex(s[i + 2]) >= 0) {
            out.push_back(static_cast<char>(hex(s[i + 1]) * 16 + hex(s[i + 2])));
            i += 2;
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

ScopeInfo interpretScopes(const std::vector<std::string>& scopes) {
    static const std::string base = "onvif://www.onvif.org/";
    ScopeInfo info;
    for (const auto& sc : scopes) {
        const std::string l = lower(sc);
        if (l.compare(0, base.size(), base) != 0) continue;
        const std::string rest = sc.substr(base.size());
        const std::string lrest = l.substr(base.size());
        auto value = [&](const char* key) { return percentDecode(rest.substr(std::string(key).size())); };
        if (lrest.compare(0, 5, "name/") == 0) { if (info.name.empty()) info.name = value("name/"); }
        else if (lrest.compare(0, 9, "hardware/") == 0) { if (info.hardware.empty()) info.hardware = value("hardware/"); }
        else if (lrest.compare(0, 9, "location/") == 0) { if (info.location.empty()) info.location = value("location/"); }
        else if (lrest.compare(0, 8, "profile/") == 0) info.profiles.push_back(percentDecode(rest.substr(8)));
        else if (lrest == "type/ptz") info.ptzHint = true;
    }
    return info;
}

bool matchesScopes(const std::vector<std::string>& scopes, const std::vector<std::string>& required) {
    for (const auto& req : required) {
        const std::string lr = lower(req);
        bool found = false;
        for (const auto& sc : scopes) {
            const std::string ls = lower(sc);
            if (ls.compare(0, lr.size(), lr) == 0) { found = true; break; }
        }
        if (!found) return false;
    }
    return true;
}

std::string ipFromXAddrs(const std::vector<std::string>& xaddrs) {
    std::string linkLocal, anyHost;
    for (const auto& xa : xaddrs) {
        auto u = hb::onvif::parseUrl(xa);
        if (!u.ok()) continue;
        const std::string& h = u.value().host;
        if (isIpv4Literal(h)) {
            if (h.compare(0, 8, "169.254.") == 0) { if (linkLocal.empty()) linkLocal = h; }
            else return h;
        } else if (anyHost.empty()) {
            anyHost = h;
        }
    }
    return !linkLocal.empty() ? linkLocal : anyHost;
}

std::string guessManufacturer(std::string_view text) {
    static const char* const kVendors[][2] = {
        {"hikvision", "Hikvision"}, {"hik", "Hikvision"}, {"ds-2", "Hikvision"}, {"dahua", "Dahua"}, {"dh-", "Dahua"},
        {"ipc-h", "Dahua"}, {"axis", "Axis"}, {"uniview", "Uniview"}, {"unv", "Uniview"}, {"reolink", "Reolink"},
        {"foscam", "Foscam"}, {"amcrest", "Amcrest"}, {"vivotek", "Vivotek"}, {"bosch", "Bosch"}, {"hanwha", "Hanwha"},
        {"samsung", "Hanwha"}, {"sony", "Sony"}, {"panasonic", "Panasonic"}, {"tp-link", "TP-Link"}, {"tapo", "TP-Link"},
        {"ubnt", "Ubiquiti"}, {"unifi", "Ubiquiti"}, {"fomako", "Fomako"}, {"lilin", "Lilin"}, {"geovision", "GeoVision"},
    };
    const std::string l = lower(text);
    for (const auto& v : kVendors)
        if (l.find(v[0]) != std::string::npos) return v[1];
    return {};
}

}  // namespace hb::discovery
