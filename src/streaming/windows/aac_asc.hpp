#pragma once
#include <cstdint>
#include <vector>

// Pure extraction of the AudioSpecificConfig (ISO/IEC 14496-3) bytes out of the blob that Media
// Foundation's AAC encoder MFT exposes via MF_MT_USER_DATA on its output media type. No COM/MFT
// dependency, so this is testable on any platform - see nal_utils.hpp's note, same reasoning.
namespace hb::win::aac {

// TODO(verify-on-windows): per MS Learn's "AAC Encoder (Media Foundation)" docs and mmreg.h's
// HEAACWAVEINFO struct, the MF_MT_USER_DATA blob is documented to be a 12-byte header -
// wPayloadType (2 bytes), wAudioProfileLevelIndication (2), wStructType (2), wReserved1 (2),
// dwReserved2 (4) = 12 - immediately followed by the raw AudioSpecificConfig() bytes. This 12-byte
// figure is taken from the documented struct layout, not confirmed against a real blob captured on
// this machine class. If extraction below yields an obviously-wrong ASC (wrong AOT/sample-rate
// index once decoded, or players reject the AAC sequence header), re-check this offset first -
// it's the single most likely spot for a wrong-but-plausible guess in this file.
constexpr std::size_t kHeaacWaveInfoHeaderSize = 12;

// Extracts just the AudioSpecificConfig tail of `userData` (the full MF_MT_USER_DATA blob) into
// `out`. False (leaving `out` untouched) if `userData` is too short to hold the header plus at
// least one ASC byte.
bool extractAudioSpecificConfig(const std::vector<std::uint8_t>& userData, std::vector<std::uint8_t>& out);

}  // namespace hb::win::aac
