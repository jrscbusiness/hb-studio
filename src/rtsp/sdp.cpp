#include "rtsp/sdp.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>

#include "rtsp/base64.hpp"

namespace hb::rtsp {

namespace {
constexpr std::size_t kMaxSdp = 256 * 1024;
constexpr std::size_t kMaxMedia = 32;

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}
bool parseInt(std::string_view s, int& out) {
    s = trim(s);
    if (s.empty() || s.size() > 9) return false;
    int v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + (c - '0');
    }
    out = v;
    return true;
}
double parseDouble(std::string_view s) {
    std::string t(trim(s));
    char* end = nullptr;
    double v = std::strtod(t.c_str(), &end);
    return (end != t.c_str() && v > 0 && v < 1000) ? v : 0;
}
}  // namespace

std::optional<SessionDescription> parseSdp(std::string_view text) {
    if (text.size() > kMaxSdp) return std::nullopt;
    SessionDescription sd;
    bool sawVersion = false;
    MediaDescription* cur = nullptr;
    std::size_t p = 0;
    while (p < text.size()) {
        auto nl = text.find('\n', p);
        std::string_view line = text.substr(p, nl == std::string_view::npos ? std::string_view::npos : nl - p);
        p = nl == std::string_view::npos ? text.size() : nl + 1;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.size() < 2 || line[1] != '=') continue;
        char t = line[0];
        std::string_view v = line.substr(2);
        if (t == 'v') {
            sawVersion = true;
        } else if (t == 's') {
            sd.sessionName = std::string(v);
        } else if (t == 'm') {
            if (sd.media.size() >= kMaxMedia) break;
            MediaDescription m;
            auto s1 = v.find(' ');
            if (s1 == std::string_view::npos) { cur = nullptr; continue; }
            m.type = lower(std::string(v.substr(0, s1)));
            auto rest = trim(v.substr(s1 + 1));
            auto s2 = rest.find(' ');
            if (s2 == std::string_view::npos) { cur = nullptr; continue; }
            auto portStr = rest.substr(0, s2);
            auto slash = portStr.find('/');
            parseInt(portStr.substr(0, slash), m.port);
            rest = trim(rest.substr(s2 + 1));
            auto s3 = rest.find(' ');
            m.proto = std::string(rest.substr(0, s3));
            if (s3 != std::string_view::npos) {
                std::string_view fmts = rest.substr(s3 + 1);
                std::size_t q = 0;
                while (q < fmts.size()) {
                    auto sp = fmts.find(' ', q);
                    int pt;
                    if (parseInt(fmts.substr(q, sp == std::string_view::npos ? std::string_view::npos : sp - q), pt) &&
                        pt >= 0 && pt <= 127)
                        m.payloadTypes.push_back(pt);
                    if (sp == std::string_view::npos) break;
                    q = sp + 1;
                }
            }
            sd.media.push_back(std::move(m));
            cur = &sd.media.back();
        } else if (t == 'a') {
            auto colon = v.find(':');
            std::string name = lower(std::string(trim(v.substr(0, colon))));
            std::string_view val = colon == std::string_view::npos ? std::string_view{} : trim(v.substr(colon + 1));
            if (name == "control") {
                (cur ? cur->control : sd.control) = std::string(val);
            } else if (!cur) {
                // session-level attributes we don't use
            } else if (name == "rtpmap") {
                auto sp = val.find(' ');
                int pt;
                if (sp != std::string_view::npos && parseInt(val.substr(0, sp), pt)) {
                    auto enc = trim(val.substr(sp + 1));
                    RtpMap rm;
                    auto s1 = enc.find('/');
                    rm.encoding = std::string(enc.substr(0, s1));
                    if (s1 != std::string_view::npos) {
                        auto r2 = enc.substr(s1 + 1);
                        auto s2 = r2.find('/');
                        parseInt(r2.substr(0, s2), rm.clockRate);
                        if (s2 != std::string_view::npos) parseInt(r2.substr(s2 + 1), rm.channels);
                    }
                    cur->rtpmap[pt] = rm;
                }
            } else if (name == "fmtp") {
                auto sp = val.find(' ');
                int pt;
                if (sp != std::string_view::npos && parseInt(val.substr(0, sp), pt))
                    cur->fmtp[pt] = std::string(trim(val.substr(sp + 1)));
            } else if (name == "framerate" || name == "x-framerate") {
                cur->framerate = parseDouble(val);
            } else if (name == "x-dimensions") {
                auto c = val.find(',');
                int w, h;
                if (c != std::string_view::npos && parseInt(val.substr(0, c), w) && parseInt(val.substr(c + 1), h)) {
                    cur->width = w;
                    cur->height = h;
                }
            } else if (name == "framesize") {  // "96 1920-1080"
                auto sp = val.find(' ');
                if (sp != std::string_view::npos) {
                    auto dim = trim(val.substr(sp + 1));
                    auto d = dim.find('-');
                    int w, h;
                    if (d != std::string_view::npos && parseInt(dim.substr(0, d), w) && parseInt(dim.substr(d + 1), h)) {
                        cur->width = w;
                        cur->height = h;
                    }
                }
            } else if (name == "cliprect") {  // "0,0,1080,1920" (top,left,bottom,right)
                int n[4], i = 0;
                std::size_t q = 0;
                while (i < 4 && q <= val.size()) {
                    auto c = val.find(',', q);
                    if (!parseInt(val.substr(q, c == std::string_view::npos ? std::string_view::npos : c - q), n[i])) break;
                    ++i;
                    if (c == std::string_view::npos) break;
                    q = c + 1;
                }
                if (i == 4 && !cur->width) {
                    cur->height = n[2] - n[0];
                    cur->width = n[3] - n[1];
                }
            }
        }
    }
    if (!sawVersion) return std::nullopt;
    return sd;
}

namespace {
std::vector<std::pair<std::string, std::string>> fmtpParams(std::string_view fmtp) {
    std::vector<std::pair<std::string, std::string>> out;
    std::size_t p = 0;
    while (p < fmtp.size()) {
        auto semi = fmtp.find(';', p);
        auto seg = trim(fmtp.substr(p, semi == std::string_view::npos ? std::string_view::npos : semi - p));
        auto eq = seg.find('=');
        if (eq != std::string_view::npos)
            out.emplace_back(lower(std::string(trim(seg.substr(0, eq)))), std::string(trim(seg.substr(eq + 1))));
        if (semi == std::string_view::npos) break;
        p = semi + 1;
    }
    return out;
}
void addB64(VideoFmtp& r, std::string_view list) {
    std::size_t p = 0;
    while (p <= list.size()) {
        auto c = list.find(',', p);
        auto item = trim(list.substr(p, c == std::string_view::npos ? std::string_view::npos : c - p));
        if (!item.empty())
            if (auto b = base64Decode(item); b && !b->empty() && b->size() < 4096) r.paramSets.push_back(std::move(*b));
        if (c == std::string_view::npos) break;
        p = c + 1;
    }
}
}  // namespace

VideoFmtp parseH264Fmtp(std::string_view fmtp) {
    VideoFmtp r;
    for (auto& [k, v] : fmtpParams(fmtp)) {
        if (k == "sprop-parameter-sets") addB64(r, v);
        else if (k == "packetization-mode") parseInt(v, r.packetizationMode);
        else if (k == "profile-level-id") r.profileLevelId = v;
    }
    return r;
}

VideoFmtp parseH265Fmtp(std::string_view fmtp) {
    VideoFmtp r;
    std::string vps, sps, pps;
    for (auto& [k, v] : fmtpParams(fmtp)) {
        if (k == "sprop-vps") vps = v;
        else if (k == "sprop-sps") sps = v;
        else if (k == "sprop-pps") pps = v;
        else if (k == "sprop-max-don-diff") parseInt(v, r.spropMaxDonDiff);
    }
    addB64(r, vps);
    addB64(r, sps);
    addB64(r, pps);
    return r;
}

std::optional<VideoSelection> selectVideo(const SessionDescription& sdp) {
    for (const auto& m : sdp.media) {
        if (m.type != "video") continue;
        for (int pt : m.payloadTypes) {
            auto it = m.rtpmap.find(pt);
            if (it == m.rtpmap.end()) continue;
            std::string enc = lower(it->second.encoding);
            VideoSelection s;
            s.media = &m;
            s.payloadType = pt;
            s.clockRate = it->second.clockRate > 0 ? it->second.clockRate : 90000;
            auto f = m.fmtp.find(pt);
            std::string_view fm = f == m.fmtp.end() ? std::string_view{} : std::string_view(f->second);
            if (enc == "h264") {
                s.codec = Codec::H264;
                s.fmtp = parseH264Fmtp(fm);
                return s;
            }
            if (enc == "h265" || enc == "hevc") {
                s.codec = Codec::H265;
                s.fmtp = parseH265Fmtp(fm);
                return s;
            }
        }
    }
    return std::nullopt;
}

}  // namespace hb::rtsp
