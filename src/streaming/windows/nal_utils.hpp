#pragma once
#include <cstdint>
#include <vector>

// Pure Annex-B NAL unit parsing - no COM/MFT dependency, so this is testable on any platform (the
// surrounding Media Foundation code that calls it only builds on Windows; this file deliberately
// does not - see tests/streaming_win_test.cpp's registration in cmake/modules/streaming_win.cmake).
namespace hb::win::nal {

// Splits a buffer holding one or more Annex-B start-code-delimited NAL units into the individual
// units, with start codes (00 00 01 / 00 00 00 01) and any trailing zero padding before the next
// start code stripped. A unit with no payload left after stripping is omitted. Tolerates
// malformed/truncated input (no leading start code, trailing garbage, empty buffer): returns
// whatever could be parsed, never throws.
std::vector<std::vector<std::uint8_t>> splitAnnexB(const std::uint8_t* data, std::size_t len);

// H.264 NAL unit type (low 5 bits of the first byte), or -1 if `nalu` is empty.
int nalUnitType(const std::vector<std::uint8_t>& nalu);

constexpr int kNalTypeIdrSlice = 5;
constexpr int kNalTypeSps = 7;
constexpr int kNalTypePps = 8;

}  // namespace hb::win::nal
