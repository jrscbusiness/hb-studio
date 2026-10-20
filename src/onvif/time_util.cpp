#include "onvif/time_util.hpp"

#include <cstdio>

namespace hb::onvif {

std::int64_t daysFromCivil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

std::optional<std::int64_t> civilToUnix(int year, int month, int day, int hour, int minute, int second) {
    if (year < 1970 || year > 9999 || month < 1 || month > 12 || day < 1 || day > 31 || hour < 0 || hour > 23 ||
        minute < 0 || minute > 59 || second < 0 || second > 60)
        return std::nullopt;
    return daysFromCivil(year, static_cast<unsigned>(month), static_cast<unsigned>(day)) * 86400 + hour * 3600 +
           minute * 60 + second;
}

std::string formatIso8601Utc(std::int64_t t) {
    std::int64_t days = t / 86400;
    std::int64_t rem = t % 86400;
    if (rem < 0) { rem += 86400; --days; }
    days += 719468;  // civil_from_days (Hinnant)
    const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(days - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const std::int64_t y = static_cast<std::int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned d = doy - (153 * mp + 2) / 5 + 1;
    const unsigned m = mp < 10 ? mp + 3 : mp - 9;
    char buf[48];
    std::snprintf(buf, sizeof buf, "%04d-%02u-%02uT%02d:%02d:%02dZ", static_cast<int>(y + (m <= 2 ? 1 : 0)), m, d,
                  static_cast<int>(rem / 3600), static_cast<int>(rem % 3600 / 60), static_cast<int>(rem % 60));
    return buf;
}

std::optional<std::int64_t> parseIso8601(std::string_view s) {
    auto num = [&](std::size_t pos, std::size_t len) -> int {
        if (pos + len > s.size()) return -1;
        int v = 0;
        for (std::size_t i = 0; i < len; ++i) {
            if (s[pos + i] < '0' || s[pos + i] > '9') return -1;
            v = v * 10 + (s[pos + i] - '0');
        }
        return v;
    };
    if (s.size() < 19 || s[4] != '-' || s[7] != '-' || (s[10] != 'T' && s[10] != ' ') || s[13] != ':' || s[16] != ':')
        return std::nullopt;
    const int Y = num(0, 4), M = num(5, 2), D = num(8, 2), h = num(11, 2), mi = num(14, 2), se = num(17, 2);
    if (Y < 0 || M < 0 || D < 0 || h < 0 || mi < 0 || se < 0) return std::nullopt;
    auto base = civilToUnix(Y, M, D, h, mi, se);
    if (!base) return std::nullopt;
    std::size_t p = 19;
    if (p < s.size() && s[p] == '.') {
        ++p;
        while (p < s.size() && s[p] >= '0' && s[p] <= '9') ++p;
    }
    if (p >= s.size() || s[p] == 'Z' || s[p] == 'z') return base;
    if (s[p] == '+' || s[p] == '-') {
        const int sign = s[p] == '+' ? 1 : -1;
        const int oh = num(p + 1, 2);
        int om = 0;
        if (p + 3 < s.size()) om = num(p + (s[p + 3] == ':' ? 4 : 3), 2);
        if (oh < 0 || om < 0) return std::nullopt;
        return *base - sign * (oh * 3600 + om * 60);
    }
    return std::nullopt;
}

}  // namespace hb::onvif
