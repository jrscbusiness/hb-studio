#pragma once
// Pure Pelco-D / Pelco-P frame builders (no I/O). Both are one-way protocols.
#include "ptz/transport.hpp"

namespace hb::ptz::pelco {

constexpr int kMaxSpeed = 0x3F;

std::uint8_t speedByte(double magnitude01);  // 0x00..0x3F (0 only for magnitude 0)

struct Command {
    std::uint8_t cmd1 = 0, cmd2 = 0, data1 = 0, data2 = 0;
};

// cmd bits (Pelco-D): cmd1: focus near 0x01, iris open 0x02, iris close 0x04.
// cmd2: right 0x02, left 0x04, up 0x08, down 0x10, zoom tele 0x20, zoom wide 0x40, focus far 0x80.
Command motion(double pan, double tilt, double zoom);
Command stopCommand();
Command focus(int dir);  // -1 near, +1 far, 0 stop
Command iris(int dir);   // +1 open, -1 close, 0 none
Command presetSet(int n);
Command presetClear(int n);
Command presetCall(int n);

// Pelco-D: FF addr c1 c2 d1 d2 checksum   (checksum = sum of bytes 1..5 mod 256), addr 1..255.
Bytes frameD(int address, const Command& c);
// Pelco-P: A0 addr c1 c2 d1 d2 AF checksum (checksum = XOR of bytes 0..6), addr 0..31.
Bytes frameP(int address, const Command& c);

}  // namespace hb::ptz::pelco
