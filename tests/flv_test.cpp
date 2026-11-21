#include <gtest/gtest.h>

#include <algorithm>

#include "streaming/flv.hpp"

using namespace hb::streaming::flv;

TEST(Flv, AvcSequenceHeaderLayout) {
    NalUnit sps = {0x67, 0x42, 0x00, 0x1E, 0xAB, 0xCD};
    NalUnit pps = {0x68, 0xCE, 0x3C, 0x80};
    auto tag = buildAvcSequenceHeader(sps, pps);
    ASSERT_GE(tag.size(), 11u + sps.size() + pps.size());
    EXPECT_EQ(tag[0], 0x17);   // keyframe (1) << 4 | AVC (7)
    EXPECT_EQ(tag[1], 0x00);   // AVCPacketType = sequence header
    EXPECT_EQ(tag[2], 0x00); EXPECT_EQ(tag[3], 0x00); EXPECT_EQ(tag[4], 0x00);   // composition time = 0
    EXPECT_EQ(tag[5], 0x01);         // configurationVersion
    EXPECT_EQ(tag[6], sps[1]);       // AVCProfileIndication, taken from the real SPS
    EXPECT_EQ(tag[7], sps[2]);
    EXPECT_EQ(tag[8], sps[3]);
    EXPECT_EQ(tag[9], 0xFF);         // reserved + lengthSizeMinusOne=3
    EXPECT_EQ(tag[10], 0xE1);        // reserved + numOfSPS=1
    const std::uint16_t spsLen = static_cast<std::uint16_t>((tag[11] << 8) | tag[12]);
    EXPECT_EQ(spsLen, sps.size());
    std::size_t off = 13;
    EXPECT_TRUE(std::equal(sps.begin(), sps.end(), tag.begin() + static_cast<long>(off)));
    off += sps.size();
    EXPECT_EQ(tag[off], 0x01);   // numOfPPS=1
    const std::uint16_t ppsLen = static_cast<std::uint16_t>((tag[off + 1] << 8) | tag[off + 2]);
    EXPECT_EQ(ppsLen, pps.size());
    off += 3;
    EXPECT_TRUE(std::equal(pps.begin(), pps.end(), tag.begin() + static_cast<long>(off)));
    EXPECT_EQ(off + pps.size(), tag.size());
}

TEST(Flv, AvcVideoTagKeyframeVsInterAndNaluLengthPrefix) {
    NalUnit idr(50, 0xAA);
    auto key = buildAvcVideoTag({idr}, true);
    EXPECT_EQ(key[0], 0x17);
    auto inter = buildAvcVideoTag({idr}, false);
    EXPECT_EQ(inter[0], 0x27);
    EXPECT_EQ(key[1], 0x01);   // AVCPacketType = NALU
    // composition time bytes [2..4] are 0 by default
    EXPECT_EQ(key[2], 0); EXPECT_EQ(key[3], 0); EXPECT_EQ(key[4], 0);
    const std::uint32_t len = (static_cast<std::uint32_t>(key[5]) << 24) | (static_cast<std::uint32_t>(key[6]) << 16) |
                              (static_cast<std::uint32_t>(key[7]) << 8) | key[8];
    EXPECT_EQ(len, idr.size());
    EXPECT_EQ(key.size(), 2u + 3u + 4u + idr.size());
}

TEST(Flv, AvcVideoTagConcatenatesMultipleNalus) {
    NalUnit a(5, 1), b(7, 2);
    auto tag = buildAvcVideoTag({a, b}, true);
    EXPECT_EQ(tag.size(), 2u + 3u + (4 + 5) + (4 + 7));
}

TEST(Flv, AvcVideoTagCompositionTimeIsEncodedBigEndian24Bit) {
    auto tag = buildAvcVideoTag({NalUnit{1}}, true, 0x010203);
    EXPECT_EQ(tag[2], 0x01); EXPECT_EQ(tag[3], 0x02); EXPECT_EQ(tag[4], 0x03);
}

TEST(Flv, AacSequenceHeaderAndRawFrame) {
    hb::streaming::flv::Bytes asc = {0x12, 0x10};   // AAC-LC, 44.1kHz, stereo (a real ASC example)
    auto seq = buildAacSequenceHeader(asc);
    ASSERT_EQ(seq.size(), 2u + asc.size());
    EXPECT_EQ(seq[0], 0xAF);
    EXPECT_EQ(seq[1], 0x00);
    EXPECT_TRUE(std::equal(asc.begin(), asc.end(), seq.begin() + 2));

    hb::streaming::flv::Bytes frame(120, 0x7A);
    auto tag = buildAacAudioTag(frame);
    ASSERT_EQ(tag.size(), 2u + frame.size());
    EXPECT_EQ(tag[0], 0xAF);
    EXPECT_EQ(tag[1], 0x01);
    EXPECT_TRUE(std::equal(frame.begin(), frame.end(), tag.begin() + 2));
}

TEST(Flv, EmptyInputsNeverCrash) {
    EXPECT_NO_THROW(buildAvcSequenceHeader({}, {}));
    EXPECT_NO_THROW(buildAvcVideoTag({}, true));
    EXPECT_NO_THROW(buildAacSequenceHeader({}));
    EXPECT_NO_THROW(buildAacAudioTag({}));
}
