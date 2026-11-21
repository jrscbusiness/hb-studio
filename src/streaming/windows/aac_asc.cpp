#include "streaming/windows/aac_asc.hpp"

namespace hb::win::aac {

bool extractAudioSpecificConfig(const std::vector<std::uint8_t>& userData, std::vector<std::uint8_t>& out) {
    if (userData.size() <= kHeaacWaveInfoHeaderSize) return false;
    out.assign(userData.begin() + static_cast<std::ptrdiff_t>(kHeaacWaveInfoHeaderSize), userData.end());
    return !out.empty();
}

}  // namespace hb::win::aac
