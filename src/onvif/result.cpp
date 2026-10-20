#include "onvif/result.hpp"

namespace hb::onvif {

const char* errorCodeName(ErrorCode code) {
    switch (code) {
        case ErrorCode::Transport: return "Transport";
        case ErrorCode::Timeout: return "Timeout";
        case ErrorCode::Http: return "Http";
        case ErrorCode::Unauthorized: return "Unauthorized";
        case ErrorCode::SoapFault: return "SoapFault";
        case ErrorCode::MalformedXml: return "MalformedXml";
        case ErrorCode::MissingField: return "MissingField";
        case ErrorCode::Unsupported: return "Unsupported";
        case ErrorCode::InvalidArgument: return "InvalidArgument";
    }
    return "Unknown";
}

}  // namespace hb::onvif
