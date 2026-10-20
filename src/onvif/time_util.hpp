#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace hb::onvif {

// Seconds since 1970-01-01T00:00:00Z, no leap seconds. Portable (no timegm/_mkgmtime).
std::int64_t daysFromCivil(int y, unsigned m, unsigned d);
std::optional<std::int64_t> civilToUnix(int year, int month, int day, int hour, int minute, int second);
std::string formatIso8601Utc(std::int64_t unixSeconds);  // "2010-09-16T07:50:45Z"
// Accepts "YYYY-MM-DDTHH:MM:SS[.fff](Z|+hh:mm|-hh:mm)"; offsets are applied. nullopt if invalid.
std::optional<std::int64_t> parseIso8601(std::string_view text);

}  // namespace hb::onvif
