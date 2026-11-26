#include "video/pixel_convert.hpp"

// SSE2 is part of the x86-64 baseline (and of MSVC x64), so no runtime dispatch is needed there; everything else
// (ARM, 32-bit x86 without SSE2) uses the scalar path. Both give byte-identical output to yuvToBgra()
// (tests/perf_pixel_convert_test.cpp checks every (y,u,v) combination through both paths).
#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#define HB_PIXCONV_SSE2 1
#include <emmintrin.h>
#endif

namespace hb::video {

namespace {

inline std::uint8_t clamp8(int x) { return static_cast<std::uint8_t>(x < 0 ? 0 : x > 255 ? 255 : x); }

struct Chroma { int r, g, b; };
inline Chroma chromaOf(int u, int v) {
    const int d = u - 128, e = v - 128;
    return {409 * e, -100 * d - 208 * e, 516 * d};
}

// Same arithmetic as yuvToBgra(), with the chroma terms computed once per U/V pair.
inline void put(std::uint8_t* d, int y, const Chroma& c) {
    const int yy = 298 * (y - 16) + 128;
    d[0] = clamp8((yy + c.b) >> 8);
    d[1] = clamp8((yy + c.g) >> 8);
    d[2] = clamp8((yy + c.r) >> 8);
    d[3] = 255;
}

#ifdef HB_PIXCONV_SSE2
// 8 pixels at once. y16: 8 luma samples as int16 lanes (0..255); cw: [u0,v0,u1,v1,u2,v2,u3,v3] as int16 lanes (each
// U/V pair feeds two neighbouring pixels). _mm_madd_epi16 forms 298*c + 409*e (etc.) exactly in 32 bits; the
// saturating packs do the 0..255 clamp.
inline void convert8(__m128i y16, __m128i cw, std::uint8_t* d) {
    const __m128i zero = _mm_setzero_si128();
    const __m128i c = _mm_sub_epi16(y16, _mm_set1_epi16(16));
    const __m128i de = _mm_sub_epi16(cw, _mm_set1_epi16(128));   // [d0,e0,d1,e1,...]
    const __m128i eDup = _mm_shufflehi_epi16(_mm_shufflelo_epi16(de, _MM_SHUFFLE(3, 3, 1, 1)), _MM_SHUFFLE(3, 3, 1, 1));
    const __m128i dDup = _mm_shufflehi_epi16(_mm_shufflelo_epi16(de, _MM_SHUFFLE(2, 2, 0, 0)), _MM_SHUFFLE(2, 2, 0, 0));
    const __m128i cdLo = _mm_unpacklo_epi16(c, dDup), cdHi = _mm_unpackhi_epi16(c, dDup);
    const __m128i ceLo = _mm_unpacklo_epi16(c, eDup), ceHi = _mm_unpackhi_epi16(c, eDup);
    const __m128i eLo = _mm_unpacklo_epi16(eDup, zero), eHi = _mm_unpackhi_epi16(eDup, zero);
    const __m128i kR = _mm_set1_epi32(298 | (409 << 16));
    const __m128i kB = _mm_set1_epi32(298 | (516 << 16));
    const __m128i kG = _mm_set1_epi32(static_cast<int>(298u | ((static_cast<unsigned>(-100) & 0xFFFFu) << 16)));
    const __m128i kE = _mm_set1_epi32(static_cast<int>(static_cast<unsigned>(-208) & 0xFFFFu));
    const __m128i round = _mm_set1_epi32(128);
    const __m128i rLo = _mm_srai_epi32(_mm_add_epi32(_mm_madd_epi16(ceLo, kR), round), 8);
    const __m128i rHi = _mm_srai_epi32(_mm_add_epi32(_mm_madd_epi16(ceHi, kR), round), 8);
    const __m128i bLo = _mm_srai_epi32(_mm_add_epi32(_mm_madd_epi16(cdLo, kB), round), 8);
    const __m128i bHi = _mm_srai_epi32(_mm_add_epi32(_mm_madd_epi16(cdHi, kB), round), 8);
    const __m128i gLo = _mm_srai_epi32(
        _mm_add_epi32(_mm_add_epi32(_mm_madd_epi16(cdLo, kG), _mm_madd_epi16(eLo, kE)), round), 8);
    const __m128i gHi = _mm_srai_epi32(
        _mm_add_epi32(_mm_add_epi32(_mm_madd_epi16(cdHi, kG), _mm_madd_epi16(eHi, kE)), round), 8);
    const __m128i r8 = _mm_packus_epi16(_mm_packs_epi32(rLo, rHi), zero);
    const __m128i g8 = _mm_packus_epi16(_mm_packs_epi32(gLo, gHi), zero);
    const __m128i b8 = _mm_packus_epi16(_mm_packs_epi32(bLo, bHi), zero);
    const __m128i bg = _mm_unpacklo_epi8(b8, g8);
    const __m128i ra = _mm_unpacklo_epi8(r8, _mm_set1_epi8(static_cast<char>(0xFF)));
    _mm_storeu_si128(reinterpret_cast<__m128i*>(d), _mm_unpacklo_epi16(bg, ra));
    _mm_storeu_si128(reinterpret_cast<__m128i*>(d + 16), _mm_unpackhi_epi16(bg, ra));
}
#endif

}  // namespace

void convertYuy2ToBgra(const std::uint8_t* src, int width, int height, int stride, std::uint8_t* dst) {
    if (!src || !dst || width <= 0 || height <= 0) return;
    for (int row = 0; row < height; ++row) {
        const std::uint8_t* s = src + static_cast<std::size_t>(row) * stride;
        std::uint8_t* d = dst + static_cast<std::size_t>(row) * width * 4;
        int x = 0;
#ifdef HB_PIXCONV_SSE2
        const __m128i lowByte = _mm_set1_epi16(0x00FF);
        for (; x + 8 <= width; x += 8, s += 16, d += 32) {
            const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s));
            convert8(_mm_and_si128(v, lowByte), _mm_srli_epi16(v, 8), d);   // Y0 U Y1 V: Y low byte, U/V high byte
        }
#endif
        for (; x + 2 <= width; x += 2, s += 4, d += 8) {
            const Chroma c = chromaOf(s[1], s[3]);
            put(d, s[0], c);
            put(d + 4, s[2], c);
        }
        if (width & 1) {   // odd width: one leftover pixel, reusing the last U/V pair
            put(d, s[0], chromaOf(s[1], s[3]));
        }
    }
}

void convertUyvyToBgra(const std::uint8_t* src, int width, int height, int stride, std::uint8_t* dst) {
    if (!src || !dst || width <= 0 || height <= 0) return;
    for (int row = 0; row < height; ++row) {
        const std::uint8_t* s = src + static_cast<std::size_t>(row) * stride;
        std::uint8_t* d = dst + static_cast<std::size_t>(row) * width * 4;
        int x = 0;
#ifdef HB_PIXCONV_SSE2
        const __m128i lowByte = _mm_set1_epi16(0x00FF);
        for (; x + 8 <= width; x += 8, s += 16, d += 32) {
            const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s));
            convert8(_mm_srli_epi16(v, 8), _mm_and_si128(v, lowByte), d);   // U Y0 V Y1: Y high byte, U/V low byte
        }
#endif
        for (; x + 2 <= width; x += 2, s += 4, d += 8) {
            const Chroma c = chromaOf(s[0], s[2]);
            put(d, s[1], c);
            put(d + 4, s[3], c);
        }
        if (width & 1) {
            put(d, s[1], chromaOf(s[0], s[2]));
        }
    }
}

void convertNv12ToBgra(const std::uint8_t* y, int strideY, const std::uint8_t* uv, int strideUV, int width,
                      int height, std::uint8_t* dst) {
    if (!y || !uv || !dst || width <= 0 || height <= 0) return;
    for (int row = 0; row < height; ++row) {
        const std::uint8_t* yRow = y + static_cast<std::size_t>(row) * strideY;
        const std::uint8_t* uvRow = uv + static_cast<std::size_t>(row / 2) * strideUV;
        std::uint8_t* d = dst + static_cast<std::size_t>(row) * width * 4;
        int x = 0;
#ifdef HB_PIXCONV_SSE2
        const __m128i zero = _mm_setzero_si128();
        for (; x + 8 <= width; x += 8, yRow += 8, uvRow += 8, d += 32) {
            const __m128i yy = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(yRow));
            const __m128i cc = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(uvRow));   // u0 v0 .. u3 v3
            convert8(_mm_unpacklo_epi8(yy, zero), _mm_unpacklo_epi8(cc, zero), d);
        }
#endif
        for (; x + 2 <= width; x += 2, yRow += 2, uvRow += 2, d += 8) {   // one UV sample feeds 2 pixels (and 2 rows)
            const Chroma c = chromaOf(uvRow[0], uvRow[1]);
            put(d, yRow[0], c);
            put(d + 4, yRow[1], c);
        }
        if (width & 1) put(d, yRow[0], chromaOf(uvRow[0], uvRow[1]));
    }
}

}  // namespace hb::video
