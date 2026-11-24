#pragma once
#include <string>
#include <string_view>

namespace hb::marketplace {

// MAJOR.MINOR.PATCH with optional -prerelease and +build (semver 2.0 ordering; build metadata is ignored).
struct SemVer {
    int major = 0, minor = 0, patch = 0;
    std::string pre;   // empty = a release (sorts after any prerelease of the same triple)
};

// Strict: no leading "v", no empty parts, each number at most 6 digits. The app's own version string
// ("0.3.0+abc1234") parses too. Returns false for anything else; never throws.
bool parseSemVer(std::string_view text, SemVer& out);

// <0, 0, >0. Only meaningful for parsed values.
int compareSemVer(const SemVer& a, const SemVer& b);

// true only if BOTH strings are valid and `candidate` is strictly newer than `installed`.
bool isNewerVersion(std::string_view candidate, std::string_view installed);

}  // namespace hb::marketplace
