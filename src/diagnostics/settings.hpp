#pragma once
// Persisted options of the pre-live checklist (checklist.json in %LOCALAPPDATA%\HBStudio).
// The upload speed test is OFF by default: it sends a small payload to an endpoint the USER chose.
#include <string>

namespace hb::diag {

struct ChecklistSettings {
    bool speedTestEnabled = false;
    std::string speedTestUrl;   // https:// only; invalid values are dropped on parse

    std::string toJson() const;
    // Never throws; hostile/invalid content leaves safe defaults (disabled, empty URL).
    static ChecklistSettings fromJson(const std::string& text);
    // {"t":"chkcfg","enabled":bool,"url":"..."}
    std::string uiJson() const;
};

}  // namespace hb::diag
