#include "onvif/soap.hpp"

#include <chrono>
#include <random>

#include "onvif/crypto.hpp"
#include "onvif/time_util.hpp"

namespace hb::onvif {

std::string xmlEscape(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default:
                // Drop control characters illegal in XML 1.0.
                if (static_cast<unsigned char>(c) >= 0x20 || c == '\t' || c == '\n' || c == '\r') out.push_back(c);
        }
    }
    return out;
}

UnixClock systemUnixClock() {
    return [] {
        return static_cast<std::int64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    };
}

NonceGenerator randomNonceGenerator() {
    return [] {
        std::random_device rd;
        std::string n(16, '\0');
        for (auto& ch : n) ch = static_cast<char>(rd() & 0xFF);
        return n;
    };
}

std::string buildSecurityHeader(const Credentials& creds, std::string_view rawNonce, std::string_view created,
                                PasswordType type) {
    std::string h;
    h += "<wsse:Security xmlns:wsse=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-secext-1.0.xsd\" "
         "xmlns:wsu=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-utility-1.0.xsd\">";
    h += "<wsse:UsernameToken>";
    h += "<wsse:Username>" + xmlEscape(creds.username) + "</wsse:Username>";
    if (type == PasswordType::Digest) {
        h += "<wsse:Password Type=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-username-token-profile-1.0#PasswordDigest\">" +
             passwordDigest(rawNonce, created, creds.password) + "</wsse:Password>";
    } else {
        h += "<wsse:Password Type=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-username-token-profile-1.0#PasswordText\">" +
             xmlEscape(creds.password) + "</wsse:Password>";
    }
    h += "<wsse:Nonce EncodingType=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-soap-message-security-1.0#Base64Binary\">" +
         base64Encode(rawNonce) + "</wsse:Nonce>";
    h += "<wsu:Created>" + xmlEscape(created) + "</wsu:Created>";
    h += "</wsse:UsernameToken></wsse:Security>";
    return h;
}

std::string buildEnvelope(std::string_view bodyXml, std::string_view securityHeader) {
    std::string e = "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                    "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\">";
    if (!securityHeader.empty()) {
        e += "<s:Header>";
        e.append(securityHeader);
        e += "</s:Header>";
    }
    e += "<s:Body>";
    e.append(bodyXml);
    e += "</s:Body></s:Envelope>";
    return e;
}

std::string SoapBuilder::build(const Message& msg, const Credentials* creds) const {
    if (!msg.requiresAuth || creds == nullptr || creds->username.empty()) return buildEnvelope(msg.bodyXml);
    const std::string created = formatIso8601Utc(clock_() + offset_);
    return buildEnvelope(msg.bodyXml, buildSecurityHeader(*creds, nonce_(), created, type_));
}

std::string soapContentType(std::string_view action) {
    return "application/soap+xml; charset=utf-8; action=\"" + std::string(action) + "\"";
}

}  // namespace hb::onvif
