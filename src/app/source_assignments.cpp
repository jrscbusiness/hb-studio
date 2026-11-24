#include "app/source_assignments.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <vector>

namespace hb::app {

std::string SourceAssignments::sanitize(const std::string& s, std::size_t maxLen) {
    std::string o;
    for (unsigned char c : s) {
        if (c < 0x20 || c == 0x7F) continue;   // no control characters (tab/newline would break the format)
        if (o.size() >= maxLen) break;
        o += static_cast<char>(c);
    }
    return o;
}

SourceAssignments::Entry SourceAssignments::get(int slot) const {
    if (slot < 0 || slot >= kSlots) return {};
    std::lock_guard lock(mutex_);
    return e_[static_cast<std::size_t>(slot)];
}

bool SourceAssignments::set(int slot, std::string id, std::string name) {
    id = sanitize(id, 512);
    name = sanitize(name, 128);
    if (slot < 0 || slot >= kSlots || id.empty()) return false;
    std::lock_guard lock(mutex_);
    auto& slotEntry = e_[static_cast<std::size_t>(slot)];
    std::string keepBg = slotEntry.bg;   // FEATURE: chroma - background settings outlive a device change
    slotEntry = Entry{};
    slotEntry.id = std::move(id);
    slotEntry.name = std::move(name);
    slotEntry.bg = std::move(keepBg);
    return true;
}

void SourceAssignments::clear(int slot) {
    if (slot < 0 || slot >= kSlots) return;
    std::lock_guard lock(mutex_);
    e_[static_cast<std::size_t>(slot)] = {};
}

bool SourceAssignments::setEnabled(int slot, bool enabled) {
    if (slot < 0 || slot >= kSlots) return false;
    std::lock_guard lock(mutex_);
    auto& e = e_[static_cast<std::size_t>(slot)];
    if (e.empty()) return false;
    e.enabled = enabled;
    return true;
}

bool SourceAssignments::setFormat(int slot, int w, int h, int fpsNum, int fpsDen) {
    if (slot < 0 || slot >= kSlots) return false;
    std::lock_guard lock(mutex_);
    auto& e = e_[static_cast<std::size_t>(slot)];
    if (e.empty()) return false;
    e.w = w > 0 ? w : 0;
    e.h = h > 0 ? h : 0;
    e.fpsNum = fpsNum > 0 ? fpsNum : 0;
    e.fpsDen = fpsDen > 0 ? fpsDen : 0;
    return true;
}

// ---- FEATURE: chroma ----
bool SourceAssignments::setBackground(int slot, const std::string& bg) {
    if (slot < 0 || slot >= kSlots) return false;
    std::string clean = sanitize(bg, 600);
    std::lock_guard lock(mutex_);
    auto& e = e_[static_cast<std::size_t>(slot)];
    if (e.empty()) return false;
    e.bg = std::move(clean);
    return true;
}
// ---- /FEATURE: chroma ----

std::string SourceAssignments::serialize() const {
    std::lock_guard lock(mutex_);
    std::ostringstream o;
    for (int i = 0; i < kSlots; ++i) {
        const auto& e = e_[static_cast<std::size_t>(i)];
        if (e.empty()) continue;
        o << i << '\t' << e.id << '\t' << e.name << '\t' << (e.enabled ? 1 : 0) << '\t' << e.w << '\t' << e.h
          << '\t' << e.fpsNum << '\t' << e.fpsDen;
        if (!e.bg.empty()) o << '\t' << e.bg;   // FEATURE: chroma (9th field, only when set)
        o << '\n';
    }
    return o.str();
}

namespace {
// A decimal non-negative int, or 0 if the field is missing/empty/not purely digits (never throws).
int parseNonNegInt(const std::string& s) {
    if (s.empty() || s.find_first_not_of("0123456789") != std::string::npos || s.size() > 9) return 0;
    return std::stoi(s);
}
}  // namespace

void SourceAssignments::parse(const std::string& text) {
    std::array<Entry, kSlots> n{};
    std::istringstream in(text);
    std::string line;
    int lines = 0;
    while (std::getline(in, line) && ++lines <= 64) {
        // Tokenize by tab into at most 9 fields; anything past the 9th is simply dropped.
        std::vector<std::string> f;
        std::size_t pos = 0;
        while (f.size() < 9) {
            const auto t = line.find('\t', pos);
            f.push_back(line.substr(pos, t == std::string::npos ? std::string::npos : t - pos));
            if (t == std::string::npos) break;
            pos = t + 1;
        }
        if (f.size() < 2) continue;   // need at least "<slot>\t<id>"
        const std::string& idxs = f[0];
        if (idxs.empty() || idxs.size() > 2 || idxs.find_first_not_of("0123456789") != std::string::npos) continue;
        const int slot = std::stoi(idxs);
        if (slot < 0 || slot >= kSlots) continue;
        std::string id = sanitize(f[1], 512);
        if (id.empty()) continue;
        Entry e;
        e.id = std::move(id);
        e.name = f.size() > 2 ? sanitize(f[2], 128) : std::string();
        e.enabled = f.size() > 3 ? f[3] != "0" : true;   // anything but an exact "0" counts as enabled
        e.w = f.size() > 4 ? parseNonNegInt(f[4]) : 0;
        e.h = f.size() > 5 ? parseNonNegInt(f[5]) : 0;
        e.fpsNum = f.size() > 6 ? parseNonNegInt(f[6]) : 0;
        e.fpsDen = f.size() > 7 ? parseNonNegInt(f[7]) : 0;
        e.bg = f.size() > 8 ? sanitize(f[8], 600) : std::string();   // FEATURE: chroma
        n[static_cast<std::size_t>(slot)] = std::move(e);
    }
    std::lock_guard lock(mutex_);
    e_ = n;
}

bool SourceAssignments::load(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    if (ss.str().size() > 256 * 1024) return false;
    parse(ss.str());
    return true;
}

bool SourceAssignments::save(const std::string& path) const {
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << serialize();
        if (!f.good()) return false;
    }
    std::remove(path.c_str());
    return std::rename(tmp.c_str(), path.c_str()) == 0;
}

}  // namespace hb::app
