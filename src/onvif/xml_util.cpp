#include "onvif/xml_util.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdlib>

namespace hb::onvif::xml {

pugi::xml_node child(pugi::xml_node parent, std::string_view local) {
    for (auto c = parent.first_child(); c; c = c.next_sibling())
        if (c.type() == pugi::node_element && localName(c.name()) == local) return c;
    return {};
}

namespace {
// Depth-limited so hostile, deeply nested documents cannot overflow the stack.
pugi::xml_node descendantImpl(pugi::xml_node parent, std::string_view local, int depth) {
    if (depth > 24) return {};
    for (auto c = parent.first_child(); c; c = c.next_sibling()) {
        if (c.type() != pugi::node_element) continue;
        if (localName(c.name()) == local) return c;
        if (auto d = descendantImpl(c, local, depth + 1)) return d;
    }
    return {};
}
}  // namespace

pugi::xml_node descendant(pugi::xml_node parent, std::string_view local) { return descendantImpl(parent, local, 0); }

pugi::xml_node path(pugi::xml_node start, std::string_view p) {
    pugi::xml_node cur = start;
    while (cur && !p.empty()) {
        const auto slash = p.find('/');
        cur = child(cur, p.substr(0, slash));
        if (slash == std::string_view::npos) break;
        p.remove_prefix(slash + 1);
    }
    return cur;
}

std::string trimmed(std::string_view s) {
    std::size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return std::string(s.substr(b, e - b));
}

std::string text(pugi::xml_node n) {
    if (!n) return {};
    return trimmed(n.text().as_string(""));
}

std::string childText(pugi::xml_node parent, std::string_view local) { return text(child(parent, local)); }

std::string attr(pugi::xml_node n, std::string_view local) {
    for (auto a = n.first_attribute(); a; a = a.next_attribute())
        if (localName(a.name()) == local) return a.value();
    return {};
}

bool parseInt(std::string_view s, int& out) {
    s = std::string_view(s);
    const std::string t = trimmed(s);
    if (t.empty()) return false;
    const char* b = t.data();
    const char* e = b + t.size();
    if (*b == '+') ++b;
    long long v = 0;
    auto r = std::from_chars(b, e, v);
    if (r.ec != std::errc() || r.ptr != e || v < -2147483647LL || v > 2147483647LL) return false;
    out = static_cast<int>(v);
    return true;
}

bool parseDouble(std::string_view s, double& out) {
    const std::string t = trimmed(s);
    if (t.empty()) return false;
    // strtod honours the C locale unless the app called setlocale; normalise the separator both ways.
    std::string u = t;
    for (auto& c : u)
        if (c == ',') c = '.';
    char* end = nullptr;
    const double v = std::strtod(u.c_str(), &end);
    if (end != u.c_str() + u.size()) {
        // Locale may expect ',': retry with the original text.
        end = nullptr;
        const double v2 = std::strtod(t.c_str(), &end);
        if (end != t.c_str() + t.size()) return false;
        out = v2;
        return true;
    }
    out = v;
    return true;
}

namespace {
bool containsNoCase(std::string_view hay, std::string_view needle) {
    auto it = std::search(hay.begin(), hay.end(), needle.begin(), needle.end(),
                          [](char a, char b) { return std::tolower((unsigned char)a) == std::tolower((unsigned char)b); });
    return it != hay.end();
}
}  // namespace

bool extractFault(pugi::xml_node body, Error& out) {
    auto fault = child(body, "Fault");
    if (!fault) return false;
    out = Error{};
    // SOAP 1.2: Code/Value, Code/Subcode/Value, Reason/Text.  SOAP 1.1: faultcode, faultstring.
    std::string code = text(path(fault, "Code/Value"));
    std::string sub;
    for (auto s = path(fault, "Code/Subcode"); s; s = child(s, "Subcode")) {
        auto v = text(child(s, "Value"));
        if (!v.empty()) sub = v;  // innermost subcode is the most specific
    }
    if (code.empty()) code = childText(fault, "faultcode");
    std::string reason = text(path(fault, "Reason/Text"));
    if (reason.empty()) reason = childText(fault, "faultstring");
    if (reason.empty()) reason = text(child(fault, "Reason"));
    out.faultCode = sub.empty() ? code : sub;
    out.faultReason = reason;
    const bool auth = containsNoCase(out.faultCode, "NotAuthorized") || containsNoCase(out.faultCode, "Unauthorized") ||
                      containsNoCase(reason, "not authorized") || containsNoCase(reason, "unauthorized") ||
                      containsNoCase(reason, "authority failure") || containsNoCase(reason, "authentication");
    out.code = auth ? ErrorCode::Unauthorized : ErrorCode::SoapFault;
    out.message = "SOAP fault " + out.faultCode + (reason.empty() ? "" : ": " + reason);
    return true;
}

Result<std::unique_ptr<Response>> openResponse(std::string_view xmlText) {
    if (xmlText.empty()) return makeError(ErrorCode::MalformedXml, "empty response body");
    auto r = std::make_unique<Response>();
    const auto parsed = r->doc.load_buffer(xmlText.data(), xmlText.size(), pugi::parse_default, pugi::encoding_auto);
    if (!parsed) return makeError(ErrorCode::MalformedXml, std::string("XML parse error: ") + parsed.description());
    auto envelope = child(r->doc, "Envelope");
    if (!envelope) return makeError(ErrorCode::MalformedXml, "no SOAP Envelope element");
    r->body = child(envelope, "Body");
    if (!r->body) return makeError(ErrorCode::MalformedXml, "no SOAP Body element");
    Error fault;
    if (extractFault(r->body, fault)) return fault;
    for (auto c = r->body.first_child(); c; c = c.next_sibling())
        if (c.type() == pugi::node_element) { r->payload = c; break; }
    if (!r->payload) return makeError(ErrorCode::MissingField, "SOAP Body is empty");
    return r;
}

}  // namespace hb::onvif::xml
