#include "marketplace/semver.hpp"

#include <algorithm>
#include <cctype>
#include <vector>

namespace hb::marketplace {
namespace {

bool number(std::string_view s, std::size_t& i, int& out) {
    const std::size_t st = i;
    long v = 0;
    while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
        v = v * 10 + (s[i] - '0');
        ++i;
        if (i - st > 6) return false;
    }
    if (i == st) return false;
    out = static_cast<int>(v);
    return true;
}

std::vector<std::string> splitDots(const std::string& s) {
    std::vector<std::string> r;
    std::string cur;
    for (char c : s) {
        if (c == '.') {
            r.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    r.push_back(cur);
    return r;
}

bool allDigits(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s)
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    return true;
}

}  // namespace

bool parseSemVer(std::string_view s, SemVer& out) {
    SemVer v;
    std::size_t i = 0;
    if (!number(s, i, v.major) || i >= s.size() || s[i] != '.') return false;
    ++i;
    if (!number(s, i, v.minor) || i >= s.size() || s[i] != '.') return false;
    ++i;
    if (!number(s, i, v.patch)) return false;
    if (i < s.size() && s[i] == '-') {
        ++i;
        std::size_t e = i;
        while (e < s.size() && s[e] != '+') ++e;
        if (e == i || e - i > 32) return false;
        for (std::size_t k = i; k < e; ++k)
            if (!(std::isalnum(static_cast<unsigned char>(s[k])) || s[k] == '.' || s[k] == '-')) return false;
        v.pre = std::string(s.substr(i, e - i));
        for (const auto& part : splitDots(v.pre))
            if (part.empty()) return false;
        i = e;
    }
    if (i < s.size()) {
        if (s[i] != '+') return false;
        ++i;
        if (i >= s.size() || s.size() - i > 32) return false;
        for (; i < s.size(); ++i)
            if (!(std::isalnum(static_cast<unsigned char>(s[i])) || s[i] == '.' || s[i] == '-')) return false;
    }
    out = v;
    return true;
}

int compareSemVer(const SemVer& a, const SemVer& b) {
    if (a.major != b.major) return a.major < b.major ? -1 : 1;
    if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
    if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
    if (a.pre.empty() != b.pre.empty()) return a.pre.empty() ? 1 : -1;
    if (a.pre.empty()) return 0;
    const auto pa = splitDots(a.pre), pb = splitDots(b.pre);
    for (std::size_t k = 0; k < pa.size() && k < pb.size(); ++k) {
        const bool na = allDigits(pa[k]), nb = allDigits(pb[k]);
        if (na && nb) {
            // numeric compare without overflow: strip leading zeros, then by length, then lexicographically
            std::string x = pa[k], y = pb[k];
            x.erase(0, std::min(x.find_first_not_of('0'), x.size() - 1));
            y.erase(0, std::min(y.find_first_not_of('0'), y.size() - 1));
            if (x.size() != y.size()) return x.size() < y.size() ? -1 : 1;
            if (x != y) return x < y ? -1 : 1;
        } else if (na != nb) {
            return na ? -1 : 1;   // numeric identifiers sort before alphanumeric ones
        } else if (pa[k] != pb[k]) {
            return pa[k] < pb[k] ? -1 : 1;
        }
    }
    if (pa.size() != pb.size()) return pa.size() < pb.size() ? -1 : 1;
    return 0;
}

bool isNewerVersion(std::string_view candidate, std::string_view installed) {
    SemVer a, b;
    if (!parseSemVer(candidate, a) || !parseSemVer(installed, b)) return false;
    return compareSemVer(a, b) > 0;
}

}  // namespace hb::marketplace
