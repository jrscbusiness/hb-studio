#pragma once
// Baseline JPEG (ITU T.81 sequential DCT, Huffman, 8 bit) encoder written for HB Studio: no external
// library. Input is BGRA8 (alpha ignored), output a complete JFIF file (SOI .. EOI) that every decoder
// understands, including the standard Annex K Huffman tables and IJG-style quality scaling.
#include <cstdint>
#include <vector>

namespace hb::rec {

struct JpegOptions {
    int quality = 85;        // 1..100 (IJG scaling of the Annex K tables)
    bool chroma420 = true;   // true: 4:2:0 (2x2 chroma subsampling), false: 4:4:4
};

class JpegEncoder {
public:
    JpegEncoder(int width, int height, JpegOptions options);
    bool valid() const { return valid_; }
    int width() const { return w_; }
    int height() const { return h_; }

    // Encodes one frame (`stride` bytes per row, top-down). `out` is replaced. Never throws.
    bool encode(const std::uint8_t* bgra, int stride, std::vector<std::uint8_t>& out);

private:
    struct HuffTable {
        std::uint8_t bits[17] = {};
        std::vector<std::uint8_t> vals;
        std::uint16_t code[256] = {};
        std::uint8_t len[256] = {};
    };
    void buildHeader();
    void convert(const std::uint8_t* bgra, int stride);
    void encodeBlock(const float* plane, int planeStride, int bx, int by, int comp, std::vector<std::uint8_t>& out);

    int w_, h_;
    JpegOptions opt_;
    bool valid_ = false;
    int padW_ = 0, padH_ = 0;       // luma plane size (multiple of the MCU)
    int cw_ = 0, ch_ = 0;           // chroma plane size
    std::uint8_t qLuma_[64], qChroma_[64];   // natural order
    float rLuma_[64], rChroma_[64];          // reciprocals (natural order)
    HuffTable dc_[2], ac_[2];
    std::vector<std::uint8_t> header_;
    std::vector<float> y_, cb_, cr_, cbFull_, crFull_;
    // bit writer state
    std::uint32_t bitAcc_ = 0;
    int bitCnt_ = 0;
    int pred_[3] = {0, 0, 0};
    void putBits(std::vector<std::uint8_t>& out, std::uint32_t value, int nbits);
    void flushBits(std::vector<std::uint8_t>& out);
};

// Convenience one-shot wrapper.
bool encodeJpegBgra(const std::uint8_t* bgra, int width, int height, int stride, const JpegOptions& options,
                    std::vector<std::uint8_t>& out);

}  // namespace hb::rec
