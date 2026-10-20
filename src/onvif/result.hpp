#pragma once
#include <string>
#include <utility>
#include <variant>

namespace hb::onvif {

enum class ErrorCode {
    Transport,        // socket/DNS/connect failure
    Timeout,
    Http,             // non-2xx HTTP without a parsable SOAP fault
    Unauthorized,     // HTTP 401/403 or SOAP NotAuthorized fault
    SoapFault,        // any other SOAP fault
    MalformedXml,     // not XML / not a SOAP envelope
    MissingField,     // well-formed but a mandatory element is absent
    Unsupported,      // device lacks the service / feature
    InvalidArgument,  // bad caller input (URL, empty token...)
};

const char* errorCodeName(ErrorCode code);

struct Error {
    ErrorCode code = ErrorCode::Transport;
    std::string message;
    int httpStatus = 0;
    std::string faultCode;    // e.g. "ter:NotAuthorized" (SOAP faults only)
    std::string faultReason;  // human readable reason text
};

inline Error makeError(ErrorCode code, std::string message) {
    Error e;
    e.code = code;
    e.message = std::move(message);
    return e;
}

struct Unit {};

// Minimal expected<T, Error> (C++20 has no std::expected).
template <class T>
class Result {
public:
    Result(T value) : v_(std::move(value)) {}      // NOLINT(google-explicit-constructor)
    Result(Error error) : v_(std::move(error)) {}  // NOLINT(google-explicit-constructor)

    bool ok() const { return std::holds_alternative<T>(v_); }
    explicit operator bool() const { return ok(); }
    const T& value() const { return std::get<T>(v_); }
    T& value() { return std::get<T>(v_); }
    const Error& error() const { return std::get<Error>(v_); }

private:
    std::variant<T, Error> v_;
};

}  // namespace hb::onvif
