#include "app/source_assignments.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>

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
    e_[static_cast<std::size_t>(slot)] = {std::move(id), std::move(name)};
    return true;
}

void SourceAssignments::clear(int slot) {
    if (slot < 0 || slot >= kSlots) return;
    std::lock_guard lock(mutex_);
    e_[static_cast<std::size_t>(slot)] = {};
}

std::string SourceAssignments::serialize() const {
    std::lock_guard lock(mutex_);
    std::ostringstream o;
    for (int i = 0; i < kSlots; ++i)
        if (!e_[static_cast<std::size_t>(i)].empty())
            o << i << '\t' << e_[static_cast<std::size_t>(i)].id << '\t' << e_[static_cast<std::size_t>(i)].name << '\n';
    return o.str();
}

void SourceAssignments::parse(const std::string& text) {
    std::array<Entry, kSlots> n{};
    std::istringstream in(text);
    std::string line;
    int lines = 0;
    while (std::getline(in, line) && ++lines <= 64) {
        const auto a = line.find('\t');
        if (a == std::string::npos) continue;
        const auto b = line.find('\t', a + 1);
        const std::string idxs = line.substr(0, a);
        if (idxs.empty() || idxs.size() > 2 || idxs.find_first_not_of("0123456789") != std::string::npos) continue;
        const int slot = std::stoi(idxs);
        if (slot < 0 || slot >= kSlots) continue;
        std::string id = sanitize(line.substr(a + 1, b == std::string::npos ? std::string::npos : b - a - 1), 512);
        std::string name = b == std::string::npos ? std::string() : sanitize(line.substr(b + 1), 128);
        if (id.empty()) continue;
        n[static_cast<std::size_t>(slot)] = {std::move(id), std::move(name)};
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
