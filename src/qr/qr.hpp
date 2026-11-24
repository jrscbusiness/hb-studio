#pragma once
// Portable QR Code (ISO/IEC 18004) encoder, model 2, versions 1..10, numeric / alphanumeric / byte
// modes, error correction L/M/Q/H. Written for HB Studio (donation / PIX / URL codes); the structure
// follows the public specification. No dependencies, never throws on bad input (returns false).
#include <cstdint>
#include <string_view>
#include <vector>

namespace hb::qr {

enum class Ecc { L = 0, M = 1, Q = 2, H = 3 };
enum class Mode { Numeric, Alphanumeric, Byte };

constexpr int kMinVersion = 1;
constexpr int kMaxVersion = 10;

struct QrCode {
    int version = 0;  // 1..10
    int size = 0;     // modules per side = 17 + 4 * version
    Ecc ecc = Ecc::L;
    int mask = -1;    // 0..7
    std::vector<std::uint8_t> modules;  // size*size, 1 = dark; row-major (y * size + x)

    bool get(int x, int y) const {
        return x >= 0 && y >= 0 && x < size && y < size && modules[static_cast<std::size_t>(y) * size + x] != 0;
    }
};

// Cheapest mode able to represent `text` (bytes are taken as-is, i.e. UTF-8).
Mode bestMode(std::string_view text);

// Encodes `text` using the smallest version in [minVersion, maxVersion] that fits. Returns false if the
// text does not fit (or is empty). `forcedMask` in 0..7 disables the penalty-based mask search (tests).
bool encode(std::string_view text, Ecc ecc, QrCode& out, int minVersion = kMinVersion,
            int maxVersion = kMaxVersion, int forcedMask = -1);

// ---- building blocks, exposed for tests ----
std::uint8_t gfMul(std::uint8_t a, std::uint8_t b);                       // GF(256), polynomial 0x11D
std::vector<std::uint8_t> rsDivisor(int degree);                          // generator polynomial (monic, leading term implicit)
std::vector<std::uint8_t> rsRemainder(const std::vector<std::uint8_t>& data, const std::vector<std::uint8_t>& divisor);
std::uint32_t formatBits(Ecc ecc, int mask);   // 15 bits, already XORed with 0x5412
std::uint32_t versionBits(int version);        // 18 bits (valid for version >= 7)
int totalCodewords(int version);               // data + ECC codewords of the symbol
int dataCodewords(int version, Ecc ecc);
int eccBlocks(int version, Ecc ecc);
// Data + ECC codewords, interleaved exactly as placed in the symbol, for an already padded data stream.
std::vector<std::uint8_t> addEccAndInterleave(const std::vector<std::uint8_t>& data, int version, Ecc ecc);

}  // namespace hb::qr
