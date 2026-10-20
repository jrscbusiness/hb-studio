#pragma once
// Pure VISCA encoding/decoding (serial and VISCA over IP framing). No I/O.
// Layouts follow the public Sony VISCA command list; NOT validated against real
// hardware (see docs/PTZ.md).
#include <cstdint>
#include <optional>
#include <vector>

#include "ptz/transport.hpp"

namespace hb::ptz::visca {

constexpr int kMaxPanSpeed = 0x18;   // 1..24
constexpr int kMaxTiltSpeed = 0x14;  // 1..20
constexpr int kMaxZoomSpeed = 7;     // 0..7

// Normalized magnitude [0,1] -> protocol speed. 0 maps to the slowest valid speed.
std::uint8_t panSpeedByte(double magnitude01);
std::uint8_t tiltSpeedByte(double magnitude01);
std::uint8_t zoomSpeedNibble(double magnitude01);

std::uint8_t addrByte(int address);  // 0x80 | address (1..7)

// ---- Commands (payload only, ending with 0xFF) ----
Bytes panTiltDrive(int address, double pan, double tilt);  // signed [-1,1]; both 0 = stop
Bytes panTiltStop(int address);
Bytes zoomDrive(int address, double zoom);  // signed [-1,1]; 0 = stop
Bytes presetSet(int address, int n);
Bytes presetRecall(int address, int n);
Bytes presetReset(int address, int n);
Bytes focusAuto(int address);
Bytes focusManual(int address);
Bytes focusNear(int address, double speed01);
Bytes focusFar(int address, double speed01);
Bytes focusStop(int address);
Bytes irisUp(int address);    // open one step
Bytes irisDown(int address);  // close one step
// Absolute position. pan/tilt are signed 16-bit raw units, zoom unsigned 16-bit raw.
Bytes panTiltAbsolute(int address, double panSpeed01, double tiltSpeed01, int panRaw, int tiltRaw);
Bytes zoomDirect(int address, int zoomRaw);
Bytes inquiryPanTiltPosition(int address);
Bytes inquiryZoomPosition(int address);
Bytes addressSetBroadcast();  // 88 30 01 FF

// ---- Replies ----
struct Message {
    enum class Kind { Ack, Completion, Error, InquiryReply, Unknown } kind = Kind::Unknown;
    int socket = 0;
    int errorCode = 0;  // for Error: 0x01 length, 0x02 syntax, 0x03 buffer full, 0x04 cancelled,
                        // 0x05 no socket, 0x41 not executable
    Bytes payload;      // full frame including terminator
};

// Parses one frame (0xFF-terminated). Never reads out of bounds.
std::optional<Message> parseMessage(const Bytes& frame);
// Splits an accumulated byte stream on 0xFF; leftover partial data stays in `buffer`.
std::vector<Bytes> extractFrames(Bytes& buffer);
bool parsePanTiltPosition(const Message& m, int& pan, int& tilt);
bool parseZoomPosition(const Message& m, int& zoom);
const char* errorName(int code);

// ---- VISCA over IP: 8-byte header (type[2] length[2] sequence[4], big-endian) ----
enum class IpType : std::uint16_t {
    Command = 0x0100,
    Inquiry = 0x0110,
    Reply = 0x0111,
    DeviceSetting = 0x0120,
    Control = 0x0200,
    ControlReply = 0x0201,
};

struct IpFrame {
    std::uint16_t type = 0;
    std::uint32_t sequence = 0;
    Bytes payload;
};

Bytes wrapIp(IpType type, std::uint32_t sequence, const Bytes& payload);
// Validates the declared length against the datagram size.
std::optional<IpFrame> unwrapIp(const Bytes& datagram);
Bytes ipSequenceReset(std::uint32_t sequence);  // Control message, payload 0x01

}  // namespace hb::ptz::visca
