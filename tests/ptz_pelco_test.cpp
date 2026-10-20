#include <gtest/gtest.h>

#include "ptz/pelco.hpp"

using namespace hb::ptz;
using namespace hb::ptz::pelco;

TEST(Pelco, SpeedMapping) {
    EXPECT_EQ(speedByte(0), 0);
    EXPECT_EQ(speedByte(1.0), 0x3F);
    EXPECT_EQ(speedByte(-1.0), 0x3F);
    EXPECT_EQ(speedByte(0.001), 1);  // any non-zero request moves
    EXPECT_EQ(speedByte(9.0), 0x3F);
}

TEST(Pelco, FrameDChecksum) {
    // Pan left at speed 0x20: FF 01 00 04 20 00 25
    Command c;
    c.cmd2 = 0x04;
    c.data1 = 0x20;
    EXPECT_EQ(frameD(1, c), (Bytes{0xFF, 0x01, 0x00, 0x04, 0x20, 0x00, 0x25}));
    // Stop: FF 01 00 00 00 00 01
    EXPECT_EQ(frameD(1, stopCommand()), (Bytes{0xFF, 0x01, 0x00, 0x00, 0x00, 0x00, 0x01}));
    // Checksum wraps modulo 256.
    Command big;
    big.cmd1 = 0xFF;
    big.cmd2 = 0xFF;
    EXPECT_EQ(frameD(0xFF, big)[6], static_cast<std::uint8_t>((0xFF + 0xFF + 0xFF) & 0xFF));
}

TEST(Pelco, FramePChecksum) {
    // Stop, address 1: A0 01 00 00 00 00 AF XOR = 0x0E
    auto f = frameP(1, stopCommand());
    EXPECT_EQ(f, (Bytes{0xA0, 0x01, 0x00, 0x00, 0x00, 0x00, 0xAF, 0x0E}));
    EXPECT_EQ(frameP(99, stopCommand())[1], 31);  // address clamped
}

TEST(Pelco, MotionBits) {
    auto m = motion(1.0, 1.0, 1.0);
    EXPECT_EQ(m.cmd2, 0x02 | 0x08 | 0x20);
    EXPECT_EQ(m.data1, 0x3F);
    EXPECT_EQ(m.data2, 0x3F);
    m = motion(-1.0, -1.0, -1.0);
    EXPECT_EQ(m.cmd2, 0x04 | 0x10 | 0x40);
    EXPECT_EQ(motion(0, 0, 0).cmd2, 0);
}

TEST(Pelco, PresetAndLens) {
    EXPECT_EQ(presetSet(7).cmd2, 0x03);
    EXPECT_EQ(presetCall(7).cmd2, 0x07);
    EXPECT_EQ(presetClear(7).cmd2, 0x05);
    EXPECT_EQ(presetCall(7).data2, 7);
    EXPECT_EQ(presetCall(900).data2, 255);
    EXPECT_EQ(focus(-1).cmd1, 0x01);
    EXPECT_EQ(focus(1).cmd2, 0x80);
    EXPECT_EQ(iris(1).cmd1, 0x02);
    EXPECT_EQ(iris(-1).cmd1, 0x04);
}
