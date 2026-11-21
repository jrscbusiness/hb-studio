#include "streaming/windows/nal_utils.hpp"

namespace hb::win::nal {

std::vector<std::vector<std::uint8_t>> splitAnnexB(const std::uint8_t* data, std::size_t len) {
    std::vector<std::vector<std::uint8_t>> units;
    if (!data || len < 3) return units;

    // Positions right after each matched 3-byte "00 00 01" start code. This 3-byte scan also
    // correctly handles 4-byte "00 00 00 01" start codes: the extra leading zero byte of a 4-byte
    // code just becomes a trailing zero byte of the *previous* unit's range, which the
    // trailing_zero_8bits stripping below removes anyway.
    std::vector<std::size_t> scStart;       // index of the leading 0 of each matched start code
    std::vector<std::size_t> payloadStart;  // index right after each matched start code
    std::size_t i = 0;
    while (i + 2 < len) {
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
            scStart.push_back(i);
            payloadStart.push_back(i + 3);
            i += 3;
        } else {
            ++i;
        }
    }
    for (std::size_t k = 0; k < payloadStart.size(); ++k) {
        const std::size_t begin = payloadStart[k];
        std::size_t end = (k + 1 < scStart.size()) ? scStart[k + 1] : len;
        while (end > begin && data[end - 1] == 0) --end;  // strip trailing_zero_8bits padding
        if (end > begin) units.emplace_back(data + begin, data + end);
    }
    return units;
}

int nalUnitType(const std::vector<std::uint8_t>& nalu) {
    if (nalu.empty()) return -1;
    return nalu[0] & 0x1F;
}

}  // namespace hb::win::nal
