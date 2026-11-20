#include "recording/jpeg_encoder.hpp"

#include <algorithm>
#include <cmath>

namespace hb::rec {

namespace {

const std::uint8_t kZigzag[64] = {0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
                                  12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
                                  35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
                                  58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

// ITU T.81 Annex K.1 quantisation tables (natural order).
const std::uint8_t kQLuma[64] = {16, 11, 10, 16, 24,  40,  51,  61,  12, 12, 14, 19, 26,  58,  60,  55,
                                 14, 13, 16, 24, 40,  57,  69,  56,  14, 17, 22, 29, 51,  87,  80,  62,
                                 18, 22, 37, 56, 68,  109, 103, 77,  24, 35, 55, 64, 81,  104, 113, 92,
                                 49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99};
const std::uint8_t kQChroma[64] = {17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99,
                                   24, 26, 56, 99, 99, 99, 99, 99, 47, 66, 99, 99, 99, 99, 99, 99,
                                   99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
                                   99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99};

// ITU T.81 Annex K.3 standard Huffman tables.
const std::uint8_t kDcLumBits[16] = {0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0};
const std::uint8_t kDcChrBits[16] = {0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0};
const std::uint8_t kDcVals[12] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
const std::uint8_t kAcLumBits[16] = {0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d};
const std::uint8_t kAcLumVals[162] = {
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07, 0x22, 0x71,
    0x14, 0x32, 0x81, 0x91, 0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0, 0x24, 0x33, 0x62, 0x72,
    0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x34, 0x35, 0x36, 0x37,
    0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59,
    0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83,
    0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3,
    0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3,
    0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2,
    0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa};
const std::uint8_t kAcChrBits[16] = {0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77};
const std::uint8_t kAcChrVals[162] = {
    0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71, 0x13, 0x22,
    0x32, 0x81, 0x08, 0x14, 0x42, 0x91, 0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0, 0x15, 0x62, 0x72, 0xd1,
    0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x35, 0x36,
    0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58,
    0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a,
    0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a,
    0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba,
    0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda,
    0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa};

// cos table: kDct[u][x] = c(u)/2 * cos((2x+1) u pi / 16)
struct DctMatrix {
    float m[8][8];
    DctMatrix() {
        const double pi = 3.14159265358979323846;
        for (int u = 0; u < 8; ++u)
            for (int x = 0; x < 8; ++x)
                m[u][x] = static_cast<float>((u == 0 ? std::sqrt(0.5) : 1.0) * 0.5 * std::cos((2 * x + 1) * u * pi / 16.0));
    }
};
const DctMatrix& dct() {
    static const DctMatrix d;
    return d;
}

void fdct8x8(const float* in, float* out) {
    const auto& C = dct().m;
    float t[64];
    for (int r = 0; r < 8; ++r)
        for (int v = 0; v < 8; ++v) {
            float s = 0;
            for (int x = 0; x < 8; ++x) s += C[v][x] * in[r * 8 + x];
            t[r * 8 + v] = s;
        }
    for (int v = 0; v < 8; ++v)
        for (int u = 0; u < 8; ++u) {
            float s = 0;
            for (int r = 0; r < 8; ++r) s += C[u][r] * t[r * 8 + v];
            out[u * 8 + v] = s;
        }
}

inline int bitLength(int v) {  // number of bits of |v| (0 for 0)
    unsigned a = static_cast<unsigned>(v < 0 ? -v : v);
    int n = 0;
    while (a) {
        ++n;
        a >>= 1;
    }
    return n;
}

void scaleTable(const std::uint8_t* base, int quality, std::uint8_t* out) {
    quality = std::clamp(quality, 1, 100);
    const int scale = quality < 50 ? 5000 / quality : 200 - quality * 2;
    for (int i = 0; i < 64; ++i) out[i] = static_cast<std::uint8_t>(std::clamp((base[i] * scale + 50) / 100, 1, 255));
}

void put16(std::vector<std::uint8_t>& v, unsigned x) {
    v.push_back(static_cast<std::uint8_t>(x >> 8));
    v.push_back(static_cast<std::uint8_t>(x));
}

}  // namespace

JpegEncoder::JpegEncoder(int width, int height, JpegOptions options) : w_(width), h_(height), opt_(options) {
    if (w_ <= 0 || h_ <= 0 || w_ > 65535 || h_ > 65535) return;
    opt_.quality = std::clamp(opt_.quality, 1, 100);
    const int mcu = opt_.chroma420 ? 16 : 8;
    padW_ = (w_ + mcu - 1) / mcu * mcu;
    padH_ = (h_ + mcu - 1) / mcu * mcu;
    cw_ = opt_.chroma420 ? padW_ / 2 : padW_;
    ch_ = opt_.chroma420 ? padH_ / 2 : padH_;
    scaleTable(kQLuma, opt_.quality, qLuma_);
    scaleTable(kQChroma, opt_.quality, qChroma_);
    for (int i = 0; i < 64; ++i) {
        rLuma_[i] = 1.0f / qLuma_[i];
        rChroma_[i] = 1.0f / qChroma_[i];
    }
    const auto build = [](HuffTable& t, const std::uint8_t* bits, const std::uint8_t* vals, std::size_t n) {
        std::size_t total = 0;
        for (int i = 0; i < 16; ++i) {
            t.bits[i + 1] = bits[i];
            total += bits[i];
        }
        if (total != n) return false;
        t.vals.assign(vals, vals + n);
        unsigned code = 0;
        std::size_t k = 0;
        for (int l = 1; l <= 16; ++l) {
            for (int i = 0; i < t.bits[l]; ++i, ++k) {
                t.code[vals[k]] = static_cast<std::uint16_t>(code);
                t.len[vals[k]] = static_cast<std::uint8_t>(l);
                ++code;
            }
            code <<= 1;
        }
        return true;
    };
    valid_ = build(dc_[0], kDcLumBits, kDcVals, 12) && build(dc_[1], kDcChrBits, kDcVals, 12) &&
             build(ac_[0], kAcLumBits, kAcLumVals, 162) && build(ac_[1], kAcChrBits, kAcChrVals, 162);
    if (!valid_) return;
    y_.assign(static_cast<std::size_t>(padW_) * static_cast<std::size_t>(padH_), 0.0f);
    cbFull_.assign(y_.size(), 0.0f);
    crFull_.assign(y_.size(), 0.0f);
    if (opt_.chroma420) {
        cb_.assign(static_cast<std::size_t>(cw_) * static_cast<std::size_t>(ch_), 0.0f);
        cr_.assign(cb_.size(), 0.0f);
    }
    buildHeader();
}

void JpegEncoder::buildHeader() {
    auto& h = header_;
    h.clear();
    h.push_back(0xFF), h.push_back(0xD8);  // SOI
    // APP0 JFIF 1.01, no density
    h.push_back(0xFF), h.push_back(0xE0);
    put16(h, 16);
    for (char c : {'J', 'F', 'I', 'F', '\0'}) h.push_back(static_cast<std::uint8_t>(c));
    h.push_back(1), h.push_back(1), h.push_back(0);
    put16(h, 1), put16(h, 1);
    h.push_back(0), h.push_back(0);
    // DQT (both tables, zigzag order)
    h.push_back(0xFF), h.push_back(0xDB);
    put16(h, 2 + 65 * 2);
    h.push_back(0x00);
    for (int i = 0; i < 64; ++i) h.push_back(qLuma_[kZigzag[i]]);
    h.push_back(0x01);
    for (int i = 0; i < 64; ++i) h.push_back(qChroma_[kZigzag[i]]);
    // SOF0
    h.push_back(0xFF), h.push_back(0xC0);
    put16(h, 17);
    h.push_back(8);
    put16(h, static_cast<unsigned>(h_));
    put16(h, static_cast<unsigned>(w_));
    h.push_back(3);
    h.push_back(1), h.push_back(opt_.chroma420 ? 0x22 : 0x11), h.push_back(0);
    h.push_back(2), h.push_back(0x11), h.push_back(1);
    h.push_back(3), h.push_back(0x11), h.push_back(1);
    // DHT x4
    const auto dht = [&](int cls, int id, const HuffTable& t) {
        h.push_back(0xFF), h.push_back(0xC4);
        put16(h, static_cast<unsigned>(2 + 1 + 16 + t.vals.size()));
        h.push_back(static_cast<std::uint8_t>((cls << 4) | id));
        for (int i = 1; i <= 16; ++i) h.push_back(t.bits[i]);
        h.insert(h.end(), t.vals.begin(), t.vals.end());
    };
    dht(0, 0, dc_[0]);
    dht(1, 0, ac_[0]);
    dht(0, 1, dc_[1]);
    dht(1, 1, ac_[1]);
    // SOS
    h.push_back(0xFF), h.push_back(0xDA);
    put16(h, 12);
    h.push_back(3);
    h.push_back(1), h.push_back(0x00);
    h.push_back(2), h.push_back(0x11);
    h.push_back(3), h.push_back(0x11);
    h.push_back(0), h.push_back(63), h.push_back(0);
}

void JpegEncoder::convert(const std::uint8_t* bgra, int stride) {
    for (int y = 0; y < padH_; ++y) {
        const std::uint8_t* row = bgra + static_cast<std::ptrdiff_t>(std::min(y, h_ - 1)) * stride;
        float* yr = &y_[static_cast<std::size_t>(y) * static_cast<std::size_t>(padW_)];
        float* cbr = &cbFull_[static_cast<std::size_t>(y) * static_cast<std::size_t>(padW_)];
        float* crr = &crFull_[static_cast<std::size_t>(y) * static_cast<std::size_t>(padW_)];
        for (int x = 0; x < padW_; ++x) {
            const std::uint8_t* p = row + static_cast<std::ptrdiff_t>(std::min(x, w_ - 1)) * 4;
            const float b = p[0], g = p[1], r = p[2];
            yr[x] = 0.299f * r + 0.587f * g + 0.114f * b - 128.0f;
            cbr[x] = -0.168736f * r - 0.331264f * g + 0.5f * b;
            crr[x] = 0.5f * r - 0.418688f * g - 0.081312f * b;
        }
    }
    if (opt_.chroma420) {
        for (int y = 0; y < ch_; ++y) {
            const float* a0 = &cbFull_[static_cast<std::size_t>(2 * y) * static_cast<std::size_t>(padW_)];
            const float* a1 = a0 + padW_;
            const float* b0 = &crFull_[static_cast<std::size_t>(2 * y) * static_cast<std::size_t>(padW_)];
            const float* b1 = b0 + padW_;
            float* cbo = &cb_[static_cast<std::size_t>(y) * static_cast<std::size_t>(cw_)];
            float* cro = &cr_[static_cast<std::size_t>(y) * static_cast<std::size_t>(cw_)];
            for (int x = 0; x < cw_; ++x) {
                cbo[x] = 0.25f * (a0[2 * x] + a0[2 * x + 1] + a1[2 * x] + a1[2 * x + 1]);
                cro[x] = 0.25f * (b0[2 * x] + b0[2 * x + 1] + b1[2 * x] + b1[2 * x + 1]);
            }
        }
    }
}

void JpegEncoder::putBits(std::vector<std::uint8_t>& out, std::uint32_t value, int nbits) {
    bitAcc_ = (bitAcc_ << nbits) | (value & ((1u << nbits) - 1));
    bitCnt_ += nbits;
    while (bitCnt_ >= 8) {
        const std::uint8_t byte = static_cast<std::uint8_t>(bitAcc_ >> (bitCnt_ - 8));
        out.push_back(byte);
        if (byte == 0xFF) out.push_back(0x00);  // byte stuffing
        bitCnt_ -= 8;
    }
    bitAcc_ &= (1u << bitCnt_) - 1;
}

void JpegEncoder::flushBits(std::vector<std::uint8_t>& out) {
    if (bitCnt_ > 0) putBits(out, 0x7F, 8 - bitCnt_);  // pad with 1 bits
    bitAcc_ = 0;
    bitCnt_ = 0;
}

void JpegEncoder::encodeBlock(const float* plane, int planeStride, int bx, int by, int comp,
                              std::vector<std::uint8_t>& out) {
    float in[64], f[64];
    for (int r = 0; r < 8; ++r) {
        const float* src = plane + static_cast<std::ptrdiff_t>(by + r) * planeStride + bx;
        for (int c = 0; c < 8; ++c) in[r * 8 + c] = src[c];
    }
    fdct8x8(in, f);
    const float* recip = comp == 0 ? rLuma_ : rChroma_;
    int zz[64];
    for (int i = 0; i < 64; ++i) zz[i] = static_cast<int>(std::lrintf(f[kZigzag[i]] * recip[kZigzag[i]]));
    const HuffTable& dc = dc_[comp == 0 ? 0 : 1];
    const HuffTable& ac = ac_[comp == 0 ? 0 : 1];
    const int diff = zz[0] - pred_[comp];
    pred_[comp] = zz[0];
    int s = bitLength(diff);
    putBits(out, dc.code[s], dc.len[s]);
    if (s) putBits(out, static_cast<std::uint32_t>(diff < 0 ? diff - 1 : diff), s);
    int run = 0;
    for (int i = 1; i < 64; ++i) {
        const int v = zz[i];
        if (v == 0) {
            ++run;
            continue;
        }
        while (run > 15) {
            putBits(out, ac.code[0xF0], ac.len[0xF0]);
            run -= 16;
        }
        s = bitLength(v);
        const int sym = (run << 4) | s;
        putBits(out, ac.code[sym], ac.len[sym]);
        putBits(out, static_cast<std::uint32_t>(v < 0 ? v - 1 : v), s);
        run = 0;
    }
    if (run > 0) putBits(out, ac.code[0x00], ac.len[0x00]);
}

bool JpegEncoder::encode(const std::uint8_t* bgra, int stride, std::vector<std::uint8_t>& out) {
    if (!valid_ || !bgra || stride < w_ * 4) return false;
    out.clear();
    try {
        convert(bgra, stride);
        out.reserve(header_.size() + static_cast<std::size_t>(w_) * static_cast<std::size_t>(h_) / 4);
        out.insert(out.end(), header_.begin(), header_.end());
        bitAcc_ = 0;
        bitCnt_ = 0;
        pred_[0] = pred_[1] = pred_[2] = 0;
        const bool c420 = opt_.chroma420;
        const int mcu = c420 ? 16 : 8;
        for (int my = 0; my < padH_; my += mcu)
            for (int mx = 0; mx < padW_; mx += mcu) {
                if (c420) {
                    encodeBlock(y_.data(), padW_, mx, my, 0, out);
                    encodeBlock(y_.data(), padW_, mx + 8, my, 0, out);
                    encodeBlock(y_.data(), padW_, mx, my + 8, 0, out);
                    encodeBlock(y_.data(), padW_, mx + 8, my + 8, 0, out);
                    encodeBlock(cb_.data(), cw_, mx / 2, my / 2, 1, out);
                    encodeBlock(cr_.data(), cw_, mx / 2, my / 2, 2, out);
                } else {
                    encodeBlock(y_.data(), padW_, mx, my, 0, out);
                    encodeBlock(cbFull_.data(), padW_, mx, my, 1, out);
                    encodeBlock(crFull_.data(), padW_, mx, my, 2, out);
                }
            }
        flushBits(out);
        out.push_back(0xFF);
        out.push_back(0xD9);  // EOI
    } catch (...) {
        out.clear();
        return false;
    }
    return true;
}

bool encodeJpegBgra(const std::uint8_t* bgra, int width, int height, int stride, const JpegOptions& options,
                    std::vector<std::uint8_t>& out) {
    JpegEncoder e(width, height, options);
    return e.valid() && e.encode(bgra, stride, out);
}

}  // namespace hb::rec
