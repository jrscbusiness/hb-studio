#pragma once
// Escapes a UTF-8 string for use between double quotes in the UI's JSON messages.
// (Behavior is exactly what win::jsonEscape always did: CR is dropped, other control chars -> \u00XX,
// bytes >= 0x80 pass through untouched.) Portable so it can be unit-tested on Linux.
#include <cstdio>
#include <string>

namespace hb::app {

inline std::string jsonEscape(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
                else o += static_cast<char>(c);
        }
    }
    return o;
}

}  // namespace hb::app
