#pragma once
// Locale-independent JSON number/string writers (std::to_chars never uses the C locale).
#include <charconv>
#include <cmath>
#include <string>
#include <string_view>

#include "core/json.hpp"

namespace hb::audio_io::json_util {

inline void num(std::string& o, double v) {
    if (!std::isfinite(v)) {
        o += '0';
        return;
    }
    char buf[40];
    const auto r = std::to_chars(buf, buf + sizeof(buf), v);
    o.append(buf, r.ptr);
}
inline void num(std::string& o, float v) {
    if (!std::isfinite(v)) {
        o += '0';
        return;
    }
    char buf[40];
    const auto r = std::to_chars(buf, buf + sizeof(buf), v);
    o.append(buf, r.ptr);
}

// dB / LUFS value with one decimal; silence (-inf, NaN, below the floor) becomes -120 (never -inf in JSON).
inline void dbNum(std::string& o, float db, float decimals = 10.0f) {
    if (!(db > -120.0f)) {
        o += "-120";
        return;
    }
    if (db > 200.0f) db = 200.0f;
    num(o, std::round(db * decimals) / decimals);
}

inline std::string quote(std::string_view s) { return "\"" + core::json::escape(s) + "\""; }

}  // namespace hb::audio_io::json_util
