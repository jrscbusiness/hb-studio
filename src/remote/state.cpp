#include "remote/state.hpp"

#include <algorithm>
#include <cmath>

#include "core/json.hpp"
#include "remote/websocket.hpp"

namespace hb::remote {

namespace {
std::string q(const std::string& s) {
    std::string t = s.size() > 64 ? s.substr(0, 64) : s;
    // Truncation may cut a UTF-8 sequence, and the host's own text might not be valid UTF-8 at all.
    while (!t.empty() && !ws::validUtf8(t)) t.pop_back();
    return "\"" + core::json::escape(t) + "\"";
}
std::string whole(double v, double lo, double hi) {
    if (!std::isfinite(v)) v = lo;
    return std::to_string(static_cast<long long>(std::llround(std::clamp(v, lo, hi))));
}
const char* b(bool v) { return v ? "true" : "false"; }
}  // namespace

std::string toJson(const RemoteState& s) {
    std::string o = "{\"t\":\"s\",\"pvw\":" + std::to_string(s.pvw) + ",\"pgm\":" + std::to_string(s.pgm) +
                    ",\"tr\":" + b(s.transitioning);
    const double p = std::isfinite(s.progress) ? std::clamp(s.progress, 0.0, 1.0) : 0.0;
    o += ",\"prog\":" + std::to_string(static_cast<int>(std::lround(p * 100.0))) + ",\"rec\":" + b(s.recording) +
         ",\"mpk\":" + whole(s.masterPeakDb, -120, 6) + ",\"src\":[";
    for (std::size_t i = 0; i < s.sources.size() && i < 32; ++i)
        o += std::string(i ? "," : "") + "{\"n\":" + q(s.sources[i].name) + ",\"st\":" + q(s.sources[i].status) + "}";
    o += "],\"aud\":[";
    for (std::size_t i = 0; i < s.strips.size() && i < 32; ++i) {
        const auto& t = s.strips[i];
        o += std::string(i ? "," : "") + "{\"n\":" + q(t.name) + ",\"db\":" + whole(t.faderDb, -120, 10) + ",\"m\":" + b(t.mute) +
             ",\"s\":" + b(t.solo) + ",\"pk\":" + whole(t.peakDb, -120, 6) + "}";
    }
    o += "],\"tx\":[";
    for (std::size_t i = 0; i < s.dests.size() && i < 32; ++i) {
        const auto& d = s.dests[i];
        o += std::string(i ? "," : "") + "{\"id\":" + q(d.id) + ",\"n\":" + q(d.name) + ",\"p\":" + q(d.platform) +
             ",\"run\":" + b(d.running) + ",\"pub\":" + b(d.publishing) + "}";
    }
    o += "]}";
    return o;
}

}  // namespace hb::remote
