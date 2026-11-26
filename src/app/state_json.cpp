#include "app/state_json.hpp"

#include <cstdio>

#include "app/json_escape.hpp"

namespace hb::app {

namespace {
const char* b(bool v) { return v ? "true" : "false"; }
}  // namespace

std::string buildStateJson(const StateSnapshot& s) {
    const auto& v = s.vcam;
    char buf[2048];
    std::snprintf(buf, sizeof buf,
                  "{\"t\":\"s\",\"pvw\":%d,\"pgm\":%d,\"transition\":%s,\"progress\":%.3f,\"fps\":%.2f,\"target\":%d,"
                  "\"pw\":%d,\"ph\":%d,"
                  "\"ver\":\"%s\",\"frames\":%llu,\"skipped\":%llu,\"tier\":\"%s\",\"cores\":%u,\"ramGiB\":%u,\"srcRec\":%d,\"srcMax\":%d,"
                  "\"vcam\":{\"supported\":%s,\"installed\":%s,\"active\":%s,\"connected\":%s,\"busy\":%s,\"error\":%s,"
                  "\"sent\":%llu,\"message\":\"%s\"}",
                  s.pvw, s.pgm, b(s.transitioning), s.progress, s.fps, s.targetFps, s.programWidth, s.programHeight,
                  s.version.c_str(), static_cast<unsigned long long>(s.frames), static_cast<unsigned long long>(s.skipped),
                  s.tier.c_str(), s.cores, s.ramGiB, s.sourcesRecommended, s.sourcesMax, b(v.supported), b(v.installed),
                  b(v.active), b(v.connected), b(v.busy), b(v.error), static_cast<unsigned long long>(v.sent),
                  jsonEscape(v.message).c_str());
    std::string j = buf;
    j += s.toastId > 0 ? ",\"toast\":{\"id\":" + std::to_string(s.toastId) + ",\"text\":\"" + jsonEscape(s.toast) + "\"}"
                       : std::string(",\"toast\":null");
    j += ",\"slots\":[";
    for (std::size_t i = 0; i < s.slots.size(); ++i) {
        const auto& sl = s.slots[i];
        char sb[160];
        std::snprintf(sb, sizeof sb, "%s{\"k\":\"%s\",\"st\":\"%s\",\"en\":%s,\"w\":%d,\"h\":%d,\"fps\":%.1f,\"name\":\"", i ? "," : "",
                      sl.kind.c_str(), sl.status.c_str(), b(sl.enabled), sl.width, sl.height, sl.fps);
        j += sb + jsonEscape(sl.label) + "\",\"id\":\"" + jsonEscape(sl.deviceId) + "\",\"msg\":\"" + jsonEscape(sl.message) + "\"}";
    }
    j += "]";
    j += ",\"log\":[";
    for (std::size_t i = 0; i < s.log.size(); ++i) {
        if (i) j += ",";
        j += "{\"t\":\"" + jsonEscape(s.log[i].text) + "\",\"l\":\"" + s.log[i].level + "\"}";
    }
    j += "]}";
    return j;
}

}  // namespace hb::app
