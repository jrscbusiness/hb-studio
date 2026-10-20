#pragma once
// Internal helpers: namespace-prefix-agnostic navigation over pugixml. Not part of the public API.
#include <pugixml.hpp>

#include <memory>
#include <string>
#include <string_view>

#include "onvif/result.hpp"

namespace hb::onvif::xml {

inline std::string_view localName(const char* qname) {
    std::string_view s(qname ? qname : "");
    const auto p = s.rfind(':');
    return p == std::string_view::npos ? s : s.substr(p + 1);
}

pugi::xml_node child(pugi::xml_node parent, std::string_view local);       // direct child, any prefix
pugi::xml_node descendant(pugi::xml_node parent, std::string_view local);  // first, depth-first
// Follows a path of local names through direct children: path("A/B/C").
pugi::xml_node path(pugi::xml_node start, std::string_view slashPath);
std::string trimmed(std::string_view s);
std::string text(pugi::xml_node n);  // trimmed text content, "" for null node
std::string childText(pugi::xml_node parent, std::string_view local);
std::string attr(pugi::xml_node n, std::string_view local);  // attribute by local name

bool parseInt(std::string_view s, int& out);
bool parseDouble(std::string_view s, double& out);  // locale independent, accepts "1.5", "-0.25", "1e-3"

// A parsed SOAP response. `payload` is the first element inside Body.
struct Response {
    pugi::xml_document doc;
    pugi::xml_node body;
    pugi::xml_node payload;
};

// Parses an envelope. Errors: MalformedXml (not XML / no Envelope / no Body),
// Unauthorized / SoapFault (Body contains a Fault).
Result<std::unique_ptr<Response>> openResponse(std::string_view xml);

// Extracts a Fault from `body` if present.
bool extractFault(pugi::xml_node body, Error& out);

}  // namespace hb::onvif::xml
