#include "diagnostics/settings.hpp"

#include "core/json.hpp"
#include "diagnostics/checklist.hpp"

namespace hb::diag {

std::string ChecklistSettings::toJson() const {
    return std::string("{\"speedTestEnabled\":") + (speedTestEnabled ? "true" : "false") + ",\"speedTestUrl\":\"" +
           core::json::escape(speedTestUrl) + "\"}";
}

std::string ChecklistSettings::uiJson() const {
    return std::string("{\"t\":\"chkcfg\",\"enabled\":") + (speedTestEnabled ? "true" : "false") + ",\"url\":\"" +
           core::json::escape(speedTestUrl) + "\"}";
}

ChecklistSettings ChecklistSettings::fromJson(const std::string& text) {
    ChecklistSettings s;
    core::json::Value v;
    core::json::Limits lim;
    lim.maxBytes = 16 * 1024;
    if (!core::json::parse(text, v, lim) || !v.isObj()) return s;
    if (const auto* u = v.get("speedTestUrl"); u && u->isStr() && isValidSpeedTestUrl(u->s)) s.speedTestUrl = u->s;
    if (const auto* e = v.get("speedTestEnabled"); e && e->isBool()) s.speedTestEnabled = e->b && !s.speedTestUrl.empty();
    return s;
}

}  // namespace hb::diag
