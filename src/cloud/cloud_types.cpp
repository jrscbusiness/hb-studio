#include "cloud/cloud_types.hpp"

#include <cctype>

namespace hb::cloud {

const char* errorKindName(ErrorKind k) {
    switch (k) {
        case ErrorKind::Transient: return "transient";
        case ErrorKind::Auth: return "auth";
        case ErrorKind::Quota: return "quota";
        case ErrorKind::SessionLost: return "session-lost";
        case ErrorKind::Local: return "local";
        case ErrorKind::Permanent: return "permanent";
        case ErrorKind::Cancelled: return "cancelled";
    }
    return "permanent";
}

std::string headerValue(const HttpResponse& r, std::string_view name) {
    for (const auto& h : r.headers) {
        if (h.first.size() != name.size()) continue;
        bool eq = true;
        for (std::size_t i = 0; i < name.size(); ++i)
            if (std::tolower(static_cast<unsigned char>(h.first[i])) != std::tolower(static_cast<unsigned char>(name[i]))) {
                eq = false;
                break;
            }
        if (eq) return h.second;
    }
    return {};
}

}  // namespace hb::cloud
