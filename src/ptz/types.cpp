#include "ptz/types.hpp"

namespace hb::ptz {

const char* statusName(PtzStatus s) {
    switch (s) {
        case PtzStatus::Ok: return "Ok";
        case PtzStatus::NotSupported: return "NotSupported";
        case PtzStatus::Timeout: return "Timeout";
        case PtzStatus::Error: return "Error";
    }
    return "?";
}

namespace {
constexpr bool PTZCapabilities::* kFlags[] = {
    &PTZCapabilities::panTilt,      &PTZCapabilities::zoom,         &PTZCapabilities::variableSpeed,
    &PTZCapabilities::presetRecall, &PTZCapabilities::presetSave,   &PTZCapabilities::presetDelete,
    &PTZCapabilities::presetList,   &PTZCapabilities::focusAuto,    &PTZCapabilities::focusManual,
    &PTZCapabilities::focusDrive,   &PTZCapabilities::iris,         &PTZCapabilities::statusInquiry, &PTZCapabilities::absoluteMove,
};
}

bool PTZCapabilities::covers(const PTZCapabilities& required) const {
    for (auto f : kFlags)
        if (required.*f && !(this->*f)) return false;
    return true;
}

int PTZCapabilities::coverCount(const PTZCapabilities& required) const {
    int n = 0;
    for (auto f : kFlags)
        if (required.*f && (this->*f)) ++n;
    return n;
}

}  // namespace hb::ptz
