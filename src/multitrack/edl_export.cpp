#include "multitrack/edl_export.hpp"

#include <algorithm>
#include <cstdio>

#include "multitrack/track_timeline.hpp"

namespace hb::multitrack {

namespace {

int nominalFps(int num, int den) {
    if (den <= 0) den = 1;
    return std::max(1, static_cast<int>((static_cast<double>(num) + den / 2.0) / den));
}

std::string two(std::uint64_t v) {
    char b[24];
    std::snprintf(b, sizeof b, "%02llu", static_cast<unsigned long long>(v));
    return b;
}

std::string reelName(const std::string& id) {
    std::string r;
    for (char c : id) {
        const bool ok = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
        if (ok) r.push_back(c);
    }
    if (r.empty()) r = "AX";
    if (r.size() > 8) r.resize(8);
    return r;
}

std::string baseName(const std::string& rel) {
    const auto p = rel.find_last_of('/');
    return p == std::string::npos ? rel : rel.substr(p + 1);
}

}  // namespace

bool usesDropFrame(int fpsNum, int fpsDen) {
    return fpsDen == 1001 && (fpsNum == 30000 || fpsNum == 60000);
}

std::string framesToTimecode(std::uint64_t frames, int fpsNum, int fpsDen, bool dropFrame) {
    const std::uint64_t nom = static_cast<std::uint64_t>(nominalFps(fpsNum, fpsDen));
    std::uint64_t f = frames;
    const bool df = dropFrame && usesDropFrame(fpsNum, fpsDen);
    if (df) {
        const std::uint64_t drop = nom / 15;                         // 2 for 30, 4 for 60
        const std::uint64_t perMin = nom * 60 - drop;                // frames in a "dropping" minute
        const std::uint64_t per10 = nom * 600 - 9 * drop;            // frames in 10 minutes
        const std::uint64_t d = f / per10, m = f % per10;
        f += 9 * drop * d;
        if (m >= drop) f += drop * ((m - drop) / perMin);
    }
    const std::uint64_t ff = f % nom;
    const std::uint64_t s = f / nom;
    const std::uint64_t ss = s % 60, mm = (s / 60) % 60, hh = s / 3600;
    return two(hh) + ":" + two(mm) + ":" + two(ss) + (df ? ";" : ":") + two(ff);
}

std::vector<EdlFile> buildSessionEdls(const SessionManifest& m, int tcFpsNum, int tcFpsDen) {
    std::vector<EdlFile> out;
    for (const ManifestTrack& t : m.tracks) {
        if (t.kind != "video" && t.kind != "audio") continue;
        const bool video = t.kind == "video";
        const int num = video && t.fpsNum > 0 ? t.fpsNum : tcFpsNum;
        const int den = video && t.fpsDen > 0 ? t.fpsDen : tcFpsDen;
        const bool df = usesDropFrame(num, den);
        std::string s;
        s += "TITLE: HB Studio " + t.id + "\r\n";
        s += std::string("FCM: ") + (df ? "DROP FRAME" : "NON-DROP FRAME") + "\r\n\r\n";
        int ev = 0;
        for (const ManifestSegment& seg : t.segments) {
            if (seg.units == 0) continue;
            ++ev;
            std::int64_t durFrames;
            if (video) {
                durFrames = static_cast<std::int64_t>(seg.units);
            } else {
                const Ns durNs = unitsToNs(seg.units, static_cast<std::uint64_t>(std::max(1, t.sampleRate)), 1);
                durFrames = nsToUnits(durNs, static_cast<std::uint64_t>(num), static_cast<std::uint64_t>(den));
            }
            if (durFrames < 1) durFrames = 1;
            const std::int64_t recIn = std::max<std::int64_t>(
                0, nsToUnits(seg.startNs, static_cast<std::uint64_t>(num), static_cast<std::uint64_t>(den)));
            const std::string srcIn = framesToTimecode(0, num, den, df);
            const std::string srcOut = framesToTimecode(static_cast<std::uint64_t>(durFrames), num, den, df);
            const std::string rIn = framesToTimecode(static_cast<std::uint64_t>(recIn), num, den, df);
            const std::string rOut = framesToTimecode(static_cast<std::uint64_t>(recIn + durFrames), num, den, df);
            char head[64];
            std::snprintf(head, sizeof head, "%03d  %-8s %-5s %-4s %-8s", ev, reelName(t.id).c_str(),
                          video ? "V" : (t.channels == 1 ? "A" : "AA"), "C", "");
            s += std::string(head) + " " + srcIn + " " + srcOut + " " + rIn + " " + rOut + "\r\n";
            s += "* FROM CLIP NAME: " + baseName(seg.file) + "\r\n";
            s += "* SOURCE FILE: " + seg.file + "\r\n\r\n";
        }
        if (ev == 0) continue;
        out.push_back({"edl/" + t.id + ".edl", s});
    }
    return out;
}

}  // namespace hb::multitrack
