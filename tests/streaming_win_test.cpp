// Windows-only build (see cmake/modules/streaming_win.cmake), but these specific tests exercise
// only pure, host-independent logic factored out of the Media Foundation encoders - no COM/MFT,
// no real hardware - unlike tests/rec_win_test.cpp's sibling round-trip tests which do need both.
#include <gtest/gtest.h>

#include "streaming/windows/aac_asc.hpp"
#include "streaming/windows/nal_utils.hpp"

using namespace hb::win;

namespace {
std::vector<std::uint8_t> bytes(std::initializer_list<int> vs) {
    std::vector<std::uint8_t> out;
    out.reserve(vs.size());
    for (int v : vs) out.push_back(static_cast<std::uint8_t>(v));
    return out;
}
}  // namespace

TEST(NalUtils, SplitsThreeByteStartCodes) {
    // SPS(7) / PPS(8) / IDR slice(5), each tiny, 3-byte start codes.
    const auto buf = bytes({0, 0, 1, 0x67, 0x42, 0, 0, 1, 0x68, 0xCE, 0, 0, 1, 0x65, 0x11, 0x22});
    const auto units = nal::splitAnnexB(buf.data(), buf.size());
    ASSERT_EQ(units.size(), 3u);
    EXPECT_EQ(nal::nalUnitType(units[0]), nal::kNalTypeSps);
    EXPECT_EQ(units[0], bytes({0x67, 0x42}));
    EXPECT_EQ(nal::nalUnitType(units[1]), nal::kNalTypePps);
    EXPECT_EQ(units[1], bytes({0x68, 0xCE}));
    EXPECT_EQ(nal::nalUnitType(units[2]), nal::kNalTypeIdrSlice);
    EXPECT_EQ(units[2], bytes({0x65, 0x11, 0x22}));
}

TEST(NalUtils, SplitsFourByteStartCodes) {
    const auto buf = bytes({0, 0, 0, 1, 0x67, 0xAA, 0, 0, 0, 1, 0x65, 0xBB, 0xCC});
    const auto units = nal::splitAnnexB(buf.data(), buf.size());
    ASSERT_EQ(units.size(), 2u);
    EXPECT_EQ(units[0], bytes({0x67, 0xAA}));
    EXPECT_EQ(units[1], bytes({0x65, 0xBB, 0xCC}));
}

TEST(NalUtils, MixesThreeAndFourByteStartCodes) {
    const auto buf = bytes({0, 0, 0, 1, 0x67, 0xAA, 0, 0, 1, 0x65, 0xBB});
    const auto units = nal::splitAnnexB(buf.data(), buf.size());
    ASSERT_EQ(units.size(), 2u);
    EXPECT_EQ(units[0], bytes({0x67, 0xAA}));
    EXPECT_EQ(units[1], bytes({0x65, 0xBB}));
}

TEST(NalUtils, StripsTrailingZeroPadding) {
    // trailing_zero_8bits before the next start code must not end up in the previous NAL.
    const auto buf = bytes({0, 0, 1, 0x67, 0xAA, 0, 0, 0, 0, 1, 0x65, 0xBB});
    const auto units = nal::splitAnnexB(buf.data(), buf.size());
    ASSERT_EQ(units.size(), 2u);
    EXPECT_EQ(units[0], bytes({0x67, 0xAA}));
    EXPECT_EQ(units[1], bytes({0x65, 0xBB}));
}

TEST(NalUtils, EmptyAndMalformedInputsAreHandledSafely) {
    EXPECT_TRUE(nal::splitAnnexB(nullptr, 0).empty());
    const auto noStartCode = bytes({0x67, 0xAA, 0xBB});
    EXPECT_TRUE(nal::splitAnnexB(noStartCode.data(), noStartCode.size()).empty());
    const auto justStartCode = bytes({0, 0, 1});
    EXPECT_TRUE(nal::splitAnnexB(justStartCode.data(), justStartCode.size()).empty());
    EXPECT_EQ(nal::nalUnitType({}), -1);
}

TEST(NalUtils, NalUnitTypeMasksLowFiveBits) {
    EXPECT_EQ(nal::nalUnitType(bytes({0x65})), nal::kNalTypeIdrSlice);  // 0b01100101 & 0x1F = 5
    EXPECT_EQ(nal::nalUnitType(bytes({0x67})), nal::kNalTypeSps);       // 0b01100111 & 0x1F = 7
    EXPECT_EQ(nal::nalUnitType(bytes({0x68})), nal::kNalTypePps);       // 0b01101000 & 0x1F = 8
}

TEST(AacAsc, ExtractsTailAfterHeaacHeader) {
    std::vector<std::uint8_t> blob(aac::kHeaacWaveInfoHeaderSize, 0);
    const auto asc = bytes({0x12, 0x10});  // a plausible 2-byte AudioSpecificConfig
    blob.insert(blob.end(), asc.begin(), asc.end());
    std::vector<std::uint8_t> out;
    ASSERT_TRUE(aac::extractAudioSpecificConfig(blob, out));
    EXPECT_EQ(out, asc);
}

TEST(AacAsc, FalseWhenBlobTooShort) {
    std::vector<std::uint8_t> out;
    EXPECT_FALSE(aac::extractAudioSpecificConfig({}, out));
    std::vector<std::uint8_t> exactlyHeaderSize(aac::kHeaacWaveInfoHeaderSize, 0);
    EXPECT_FALSE(aac::extractAudioSpecificConfig(exactlyHeaderSize, out));
}
