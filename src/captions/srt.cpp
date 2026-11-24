#include "captions/srt.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace hb::captions {

std::string formatSrtTime(std::int64_t ms) {
    if (ms < 0) ms = 0;
    const std::int64_t h = ms / 3600000, m = ms / 60000 % 60, s = ms / 1000 % 60, f = ms % 1000;
    char buf[32];
    std::snprintf(buf, sizeof buf, "%02lld:%02lld:%02lld,%03lld", static_cast<long long>(h), static_cast<long long>(m),
                  static_cast<long long>(s), static_cast<long long>(f));
    return buf;
}

namespace {
// Removes CRs and blank lines (a blank line would terminate the cue for SRT readers).
std::string sanitize(const std::string& text) {
    std::string out, line;
    auto flushLine = [&] {
        std::size_t b = line.find_first_not_of(" \t");
        if (b != std::string::npos) {
            const std::size_t e = line.find_last_not_of(" \t");
            if (!out.empty()) out.push_back('\n');
            out += line.substr(b, e - b + 1);
        }
        line.clear();
    };
    for (char c : text) {
        if (c == '\r') continue;
        if (c == '\n') flushLine();
        else line.push_back(c);
    }
    flushLine();
    return out;
}
}  // namespace

void SrtBuilder::add(std::int64_t startMs, std::int64_t endMs, const std::string& text) {
    std::string t = sanitize(text);
    if (t.empty()) return;
    if (startMs < 0) startMs = 0;
    if (!cues_.empty() && startMs < cues_.back().endMs) startMs = cues_.back().endMs;
    std::int64_t dur = endMs - startMs;
    dur = std::max(dur, minDurationMs);
    dur = std::min(dur, std::max(maxDurationMs, minDurationMs));
    cues_.push_back({startMs, startMs + dur, std::move(t)});
}

std::string SrtBuilder::str() const {
    std::string out;
    int n = 1;
    for (const auto& c : cues_) {
        out += std::to_string(n++) + "\r\n" + formatSrtTime(c.startMs) + " --> " + formatSrtTime(c.endMs) + "\r\n";
        std::string t = c.text;
        for (std::size_t p = 0; (p = t.find('\n', p)) != std::string::npos; p += 2) t.replace(p, 1, "\r\n");
        out += t + "\r\n\r\n";
    }
    return out;
}

bool saveSrtFile(const std::string& pathUtf8, const std::string& content) {
    if (pathUtf8.empty()) return false;
    namespace fs = std::filesystem;
    try {
        const fs::path target(reinterpret_cast<const char8_t*>(pathUtf8.c_str()));
        fs::path tmp = target;
        tmp += ".tmp";
        std::error_code ec;
        if (target.has_parent_path()) fs::create_directories(target.parent_path(), ec);
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            if (!f) return false;
            f.write(content.data(), static_cast<std::streamsize>(content.size()));
            if (!f) return false;
        }
        fs::rename(tmp, target, ec);   // replaces an existing file on both POSIX and Windows (std::filesystem)
        if (ec) { fs::remove(tmp, ec); return false; }
        return true;
    } catch (...) {
        return false;
    }
}

}  // namespace hb::captions
