#include "multitrack/session_manifest.hpp"

#include <cmath>
#include <cstdio>
#include <sstream>

#include "core/json.hpp"

namespace hb::multitrack {

namespace {

// Days since 1970-01-01 -> civil date (Howard Hinnant's algorithm), valid for the proleptic Gregorian calendar.
void civilFromDays(std::int64_t z, int& y, unsigned& m, unsigned& d) {
    z += 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const std::int64_t yy = static_cast<std::int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    y = static_cast<int>(yy + (m <= 2 ? 1 : 0));
}

std::string q(const std::string& s) { return "\"" + core::json::escape(s) + "\""; }

std::string num(double v) {
    if (!std::isfinite(v)) return "0";
    char b[48];
    std::snprintf(b, sizeof b, "%.3f", v);
    return b;
}

}  // namespace

std::string unixMsToIsoUtc(std::int64_t ms) {
    std::int64_t secs = ms / 1000;
    std::int64_t rem = ms % 1000;
    if (rem < 0) {
        rem += 1000;
        --secs;
    }
    std::int64_t days = secs / 86400;
    std::int64_t sod = secs % 86400;
    if (sod < 0) {
        sod += 86400;
        --days;
    }
    int y;
    unsigned mo, d;
    civilFromDays(days, y, mo, d);
    char b[40];
    std::snprintf(b, sizeof b, "%04d-%02u-%02uT%02d:%02d:%02d.%03dZ", y, mo, d, static_cast<int>(sod / 3600),
                  static_cast<int>(sod % 3600 / 60), static_cast<int>(sod % 60), static_cast<int>(rem));
    return b;
}

std::string manifestToJson(const SessionManifest& m) {
    std::ostringstream o;
    o << "{\n";
    o << "  \"schema\": " << m.schema << ",\n";
    o << "  \"app\": " << q(m.app) << ",\n";
    o << "  \"appVersion\": " << q(m.appVersion) << ",\n";
    o << "  \"session\": " << q(m.sessionName) << ",\n";
    o << "  \"state\": " << q(m.state) << ",\n";
    o << "  \"startWallClockUtc\": " << q(unixMsToIsoUtc(m.startWallUnixMs)) << ",\n";
    o << "  \"startWallClockUnixMs\": " << m.startWallUnixMs << ",\n";
    o << "  \"monotonicT0Ns\": " << m.monotonicT0Ns << ",\n";
    o << "  \"durationNs\": " << m.durationNs << ",\n";
    o << "  \"rotationSeconds\": " << m.rotationSeconds << ",\n";
    o << "  \"syncNote\": " << q(m.syncNote) << ",\n";
    o << "  \"warnings\": [";
    for (std::size_t i = 0; i < m.warnings.size(); ++i) o << (i ? ", " : "") << q(m.warnings[i]);
    o << "],\n  \"tracks\": [";
    for (std::size_t i = 0; i < m.tracks.size(); ++i) {
        const ManifestTrack& t = m.tracks[i];
        o << (i ? ",\n    {" : "\n    {");
        o << "\n      \"id\": " << q(t.id) << ", \"kind\": " << q(t.kind) << ", \"name\": " << q(t.name)
          << ", \"device\": " << q(t.device) << ", \"codec\": " << q(t.codec) << ", \"sourceIndex\": " << t.sourceIndex << ",";
        if (t.kind == "audio")
            o << "\n      \"sampleRate\": " << t.sampleRate << ", \"channels\": " << t.channels << ", \"bitsPerSample\": "
              << t.bitsPerSample << ", \"tap\": " << q(t.tap) << ",";
        else
            o << "\n      \"width\": " << t.width << ", \"height\": " << t.height << ", \"fpsNum\": " << t.fpsNum
              << ", \"fpsDen\": " << t.fpsDen << ",";
        o << "\n      \"state\": " << q(t.state) << ", \"error\": " << q(t.error) << ",";
        o << "\n      \"startOffsetNs\": " << t.startOffsetNs << ", \"leadingSilenceFrames\": " << t.leadingSilenceFrames << ",";
        o << "\n      \"units\": " << t.units << ", \"repeated\": " << t.repeated << ", \"droppedQueue\": " << t.droppedQueue
          << ", \"droppedTimeline\": " << t.droppedTimeline << ", \"silenceFrames\": " << t.silenceFrames
          << ", \"jumped\": " << t.jumped << ", \"driftPpm\": " << num(t.driftPpm) << ",";
        o << "\n      \"segments\": [";
        for (std::size_t k = 0; k < t.segments.size(); ++k) {
            const ManifestSegment& s = t.segments[k];
            o << (k ? ",\n        {" : "\n        {") << "\"index\": " << s.index << ", \"file\": " << q(s.file)
              << ", \"startNs\": " << s.startNs << ", \"startUnit\": " << s.startUnit << ", \"units\": " << s.units
              << ", \"bytes\": " << s.bytes << ", \"closed\": " << (s.closed ? "true" : "false") << "}";
        }
        o << (t.segments.empty() ? "]" : "\n      ]") << "\n    }";
    }
    o << (m.tracks.empty() ? "]\n" : "\n  ]\n") << "}\n";
    return o.str();
}

}  // namespace hb::multitrack
