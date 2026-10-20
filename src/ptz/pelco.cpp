#include "ptz/pelco.hpp"

#include <algorithm>
#include <cmath>

namespace hb::ptz::pelco {

std::uint8_t speedByte(double m) {
    m = std::fabs(m);
    if (!(m == m) || m <= 0) return 0;
    if (m > 1) m = 1;
    return static_cast<std::uint8_t>(std::max(1L, std::lround(m * kMaxSpeed)));
}

Command motion(double pan, double tilt, double zoom) {
    Command c;
    if (pan > 0) c.cmd2 |= 0x02;
    if (pan < 0) c.cmd2 |= 0x04;
    if (tilt > 0) c.cmd2 |= 0x08;
    if (tilt < 0) c.cmd2 |= 0x10;
    if (zoom > 0) c.cmd2 |= 0x20;
    if (zoom < 0) c.cmd2 |= 0x40;
    c.data1 = speedByte(pan);
    c.data2 = speedByte(tilt);
    return c;
}
Command stopCommand() { return {}; }
Command focus(int dir) {
    Command c;
    if (dir < 0) c.cmd1 |= 0x01;
    if (dir > 0) c.cmd2 |= 0x80;
    return c;
}
Command iris(int dir) {
    Command c;
    if (dir > 0) c.cmd1 |= 0x02;
    if (dir < 0) c.cmd1 |= 0x04;
    return c;
}
static Command preset(std::uint8_t op, int n) {
    Command c;
    c.cmd2 = op;
    c.data2 = static_cast<std::uint8_t>(std::clamp(n, 0, 255));
    return c;
}
Command presetSet(int n) { return preset(0x03, n); }
Command presetClear(int n) { return preset(0x05, n); }
Command presetCall(int n) { return preset(0x07, n); }

Bytes frameD(int address, const Command& c) {
    Bytes b{0xFF, static_cast<std::uint8_t>(std::clamp(address, 0, 255)), c.cmd1, c.cmd2, c.data1, c.data2, 0};
    unsigned sum = 0;
    for (int i = 1; i <= 5; ++i) sum += b[i];
    b[6] = static_cast<std::uint8_t>(sum & 0xFF);
    return b;
}

Bytes frameP(int address, const Command& c) {
    Bytes b{0xA0, static_cast<std::uint8_t>(std::clamp(address, 0, 31)), c.cmd1, c.cmd2, c.data1, c.data2, 0xAF, 0};
    std::uint8_t x = 0;
    for (int i = 0; i < 7; ++i) x ^= b[i];
    b[7] = x;
    return b;
}

}  // namespace hb::ptz::pelco
