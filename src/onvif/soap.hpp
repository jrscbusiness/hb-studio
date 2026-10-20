#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include "onvif/types.hpp"

namespace hb::onvif {

// ONVIF / SOAP namespaces.
namespace ns {
inline constexpr const char* kDevice = "http://www.onvif.org/ver10/device/wsdl";
inline constexpr const char* kMedia = "http://www.onvif.org/ver10/media/wsdl";
inline constexpr const char* kMedia2 = "http://www.onvif.org/ver20/media/wsdl";
inline constexpr const char* kPtz = "http://www.onvif.org/ver20/ptz/wsdl";
inline constexpr const char* kImaging = "http://www.onvif.org/ver20/imaging/wsdl";
inline constexpr const char* kEvents = "http://www.onvif.org/ver10/events/wsdl";
inline constexpr const char* kSchema = "http://www.onvif.org/ver10/schema";
}  // namespace ns

// A request body ready to be wrapped into an envelope.
struct Message {
    std::string action;    // full SOAPAction URI
    std::string bodyXml;   // the single element inside <s:Body>
    bool requiresAuth = true;  // GetSystemDateAndTime is sent unauthenticated
};

std::string xmlEscape(std::string_view text);

enum class PasswordType { Digest, Text };

// Provides the current wall-clock time as Unix seconds (injectable for tests).
using UnixClock = std::function<std::int64_t()>;
// Returns 16 raw (not Base64) random bytes (injectable for tests).
using NonceGenerator = std::function<std::string()>;

UnixClock systemUnixClock();
NonceGenerator randomNonceGenerator();

// WS-Security UsernameToken header (the <wsse:Security> element only).
std::string buildSecurityHeader(const Credentials& creds, std::string_view rawNonce, std::string_view created,
                                PasswordType type = PasswordType::Digest);

// Wraps `bodyXml` in a SOAP 1.2 envelope; `securityHeader` may be empty.
std::string buildEnvelope(std::string_view bodyXml, std::string_view securityHeader = {});

// Builds authenticated envelopes. The camera clock offset (device - local, seconds) is applied to
// the `Created` timestamp; wrong time is the classic cause of WS-Security failures.
class SoapBuilder {
public:
    SoapBuilder(UnixClock clock = systemUnixClock(), NonceGenerator nonce = randomNonceGenerator())
        : clock_(std::move(clock)), nonce_(std::move(nonce)) {}

    void setClockOffsetSeconds(std::int64_t offset) { offset_ = offset; }
    std::int64_t clockOffsetSeconds() const { return offset_; }
    void setPasswordType(PasswordType t) { type_ = t; }

    // Credentials are only used when the message requires auth.
    std::string build(const Message& msg, const Credentials* creds) const;

private:
    UnixClock clock_;
    NonceGenerator nonce_;
    std::int64_t offset_ = 0;
    PasswordType type_ = PasswordType::Digest;
};

// Content-Type header value for a SOAP 1.2 POST.
std::string soapContentType(std::string_view action);

}  // namespace hb::onvif
