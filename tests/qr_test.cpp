// QR encoder tests. No decoder library is available, so correctness is checked three independent ways:
// (1) known vectors from the spec / public worked examples (Reed-Solomon, format and version bits,
// data-capacity table), (2) an independent read-back of the produced symbol (function-pattern geometry
// re-derived here from the spec, format info decoded by brute force, unmasking, zig-zag read,
// de-interleaving, Reed-Solomon syndromes evaluated at alpha^i == 0, payload parsed back), and
// (3) structural invariants. Not validated with a phone camera in this environment.
#include <gtest/gtest.h>

#include <string>

#include "qr/qr.hpp"

using namespace hb::qr;

namespace {

std::uint8_t alphaPow(int n) {
    std::uint8_t r = 1;
    for (int i = 0; i < n; ++i) r = gfMul(r, 2);
    return r;
}

bool isFunction(int v, int n, int x, int y) {
    if (x < 8 && y < 8) return true;
    if (x >= n - 8 && y < 8) return true;
    if (x < 8 && y >= n - 8) return true;
    if (y == 8 && (x < 9 || x >= n - 8)) return true;
    if (x == 8 && (y < 9 || y >= n - 8)) return true;
    if (x == 6 || y == 6) return true;
    if (v >= 7 && ((x >= n - 11 && x <= n - 9 && y < 6) || (y >= n - 11 && y <= n - 9 && x < 6))) return true;
    if (v >= 2) {
        static const int centers[11][3] = {{0}, {0}, {6, 18, 0}, {6, 22, 0}, {6, 26, 0}, {6, 30, 0}, {6, 34, 0},
                                           {6, 22, 38}, {6, 24, 42}, {6, 26, 46}, {6, 28, 50}};
        const int cnt = v >= 7 ? 3 : 2;
        for (int i = 0; i < cnt; ++i)
            for (int j = 0; j < cnt; ++j) {
                if ((i == 0 && j == 0) || (i == 0 && j == cnt - 1) || (i == cnt - 1 && j == 0)) continue;
                if (std::abs(x - centers[v][i]) <= 2 && std::abs(y - centers[v][j]) <= 2) return true;
            }
    }
    return false;
}

struct Decoded {
    bool ok = false;
    Ecc ecc = Ecc::L;
    int mask = -1;
    std::string text;
};

Decoded readBack(const QrCode& q) {
    Decoded d;
    const int n = q.size, v = q.version;
    // format info (first copy), brute force the 32 valid words
    std::uint32_t f = 0;
    for (int i = 0; i <= 5; ++i) f |= static_cast<std::uint32_t>(q.get(8, i)) << i;
    f |= static_cast<std::uint32_t>(q.get(8, 7)) << 6;
    f |= static_cast<std::uint32_t>(q.get(8, 8)) << 7;
    f |= static_cast<std::uint32_t>(q.get(7, 8)) << 8;
    for (int i = 9; i < 15; ++i) f |= static_cast<std::uint32_t>(q.get(14 - i, 8)) << i;
    // second copy must match
    std::uint32_t g = 0;
    for (int i = 0; i < 8; ++i) g |= static_cast<std::uint32_t>(q.get(n - 1 - i, 8)) << i;
    for (int i = 8; i < 15; ++i) g |= static_cast<std::uint32_t>(q.get(8, n - 15 + i)) << i;
    if (f != g || !q.get(8, n - 8)) return d;
    bool found = false;
    for (int e = 0; e < 4 && !found; ++e)
        for (int m = 0; m < 8; ++m)
            if (formatBits(static_cast<Ecc>(e), m) == f) { d.ecc = static_cast<Ecc>(e); d.mask = m; found = true; break; }
    if (!found) return d;
    if (v >= 7) {
        std::uint32_t vb = 0;
        for (int i = 0; i < 18; ++i) vb |= static_cast<std::uint32_t>(q.get(n - 11 + i % 3, i / 3)) << i;
        if (vb != versionBits(v)) return d;
    }
    auto maskFn = [&](int x, int y) {
        switch (d.mask) {
            case 0: return (x + y) % 2 == 0;
            case 1: return y % 2 == 0;
            case 2: return x % 3 == 0;
            case 3: return (x + y) % 3 == 0;
            case 4: return (x / 3 + y / 2) % 2 == 0;
            case 5: return (x * y) % 2 + (x * y) % 3 == 0;
            case 6: return ((x * y) % 2 + (x * y) % 3) % 2 == 0;
            default: return ((x + y) % 2 + (x * y) % 3) % 2 == 0;
        }
    };
    std::vector<int> bits;
    for (int right = n - 1; right >= 1; right -= 2) {
        if (right == 6) right = 5;
        const bool up = ((right + 1) & 2) == 0;
        for (int t = 0; t < n; ++t)
            for (int j = 0; j < 2; ++j) {
                const int x = right - j, y = up ? n - 1 - t : t;
                if (isFunction(v, n, x, y)) continue;
                bits.push_back(q.get(x, y) ^ maskFn(x, y));
            }
    }
    const int total = totalCodewords(v);
    if (static_cast<int>(bits.size()) < total * 8) return d;
    std::vector<std::uint8_t> cw(static_cast<std::size_t>(total), 0);
    for (int i = 0; i < total * 8; ++i) cw[static_cast<std::size_t>(i / 8)] = static_cast<std::uint8_t>(cw[static_cast<std::size_t>(i / 8)] | (bits[static_cast<std::size_t>(i)] << (7 - i % 8)));
    // de-interleave
    const int blocks = eccBlocks(v, q.ecc), dataTotal = dataCodewords(v, q.ecc);
    const int eccLen = (total - dataTotal) / blocks, shortLen = total / blocks, numShort = blocks - total % blocks;
    std::vector<std::vector<std::uint8_t>> blk(static_cast<std::size_t>(blocks));
    std::size_t k = 0;
    for (int i = 0; i <= shortLen - eccLen; ++i)
        for (int b = 0; b < blocks; ++b)
            if (i < shortLen - eccLen + (b < numShort ? 0 : 1)) blk[static_cast<std::size_t>(b)].push_back(cw[k++]);
    for (int i = 0; i < eccLen; ++i)
        for (int b = 0; b < blocks; ++b) blk[static_cast<std::size_t>(b)].push_back(cw[k++]);
    std::vector<std::uint8_t> data;
    for (auto& b : blk) {
        for (int i = 0; i < eccLen; ++i) {  // syndrome S_i = codeword polynomial evaluated at alpha^i
            const std::uint8_t a = alphaPow(i);
            std::uint8_t s = 0;
            for (std::uint8_t c : b) s = static_cast<std::uint8_t>(gfMul(s, a) ^ c);
            if (s != 0) return d;
        }
        data.insert(data.end(), b.begin(), b.end() - eccLen);
    }
    // parse payload
    std::size_t pos = 0;
    auto take = [&](int cnt) {
        unsigned r = 0;
        for (int i = 0; i < cnt; ++i, ++pos) r = (r << 1) | ((data[pos >> 3] >> (7 - (pos & 7))) & 1u);
        return r;
    };
    const unsigned mode = take(4);
    const int small = v <= 9;
    static const char* kAl = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ $%*+-./:";
    if (mode == 4) {
        const unsigned c = take(small ? 8 : 16);
        for (unsigned i = 0; i < c; ++i) d.text += static_cast<char>(take(8));
    } else if (mode == 2) {
        const unsigned c = take(small ? 9 : 11);
        for (unsigned i = 0; i + 1 < c; i += 2) { const unsigned x = take(11); d.text += kAl[x / 45]; d.text += kAl[x % 45]; }
        if (c % 2) d.text += kAl[take(6)];
    } else if (mode == 1) {
        const unsigned c = take(small ? 10 : 12);
        for (unsigned i = 0; i < c; i += 3) {
            const unsigned left = std::min(3u, c - i);
            const unsigned x = take(static_cast<int>(left) * 3 + 1);
            std::string s = std::to_string(x);
            while (s.size() < left) s = "0" + s;
            d.text += s;
        }
    } else {
        return d;
    }
    d.ok = true;
    return d;
}

}  // namespace

TEST(QrVectors, ReedSolomonHelloWorld1M) {
    // Widely published worked example: "HELLO WORLD", version 1-M.
    const std::vector<std::uint8_t> data = {32, 91, 11, 120, 209, 114, 220, 77, 67, 64, 236, 17, 236, 17, 236, 17};
    const std::vector<std::uint8_t> expect = {196, 35, 39, 119, 235, 215, 231, 226, 93, 23};
    EXPECT_EQ(rsRemainder(data, rsDivisor(10)), expect);
}

TEST(QrVectors, ReedSolomonIsoExample01234567) {
    // ISO/IEC 18004 worked example: "01234567", version 1-M.
    const std::vector<std::uint8_t> data = {16, 32, 12, 86, 97, 128, 236, 17, 236, 17, 236, 17, 236, 17, 236, 17};
    const std::vector<std::uint8_t> expect = {165, 36, 212, 193, 237, 54, 199, 135, 44, 85};
    EXPECT_EQ(rsRemainder(data, rsDivisor(10)), expect);
}

TEST(QrVectors, GaloisField) {
    EXPECT_EQ(gfMul(2, 0x80), 0x1D);  // x * x^7 = x^8 = x^4+x^3+x^2+1 (poly 0x11D)
    EXPECT_EQ(gfMul(0, 77), 0);
    EXPECT_EQ(gfMul(1, 77), 77);
    EXPECT_EQ(gfMul(3, 7), gfMul(7, 3));
    EXPECT_EQ(rsDivisor(2), (std::vector<std::uint8_t>{3, 2}));  // (x-1)(x-2) = x^2 + 3x + 2
}

TEST(QrVectors, FormatAndVersionBits) {
    EXPECT_EQ(formatBits(Ecc::M, 0), 0x5412u);
    EXPECT_EQ(formatBits(Ecc::L, 0), 0x77C4u);
    EXPECT_EQ(formatBits(Ecc::Q, 0), 0x355Fu);
    EXPECT_EQ(formatBits(Ecc::H, 0), 0x1689u);
}

TEST(QrVectors, VersionInformation) {
    EXPECT_EQ(versionBits(7), 0x07C94u);
    EXPECT_EQ(versionBits(8), 0x085BCu);
    EXPECT_EQ(versionBits(9), 0x09A99u);
    EXPECT_EQ(versionBits(10), 0x0A4D3u);
}

TEST(QrVectors, DataCapacityTable) {
    const int cap[4][10] = {{19, 34, 55, 80, 108, 136, 156, 194, 232, 274},
                            {16, 28, 44, 64, 86, 108, 124, 154, 182, 216},
                            {13, 22, 34, 48, 62, 76, 88, 110, 132, 154},
                            {9, 16, 26, 36, 46, 60, 66, 86, 100, 122}};
    const int total[10] = {26, 44, 70, 100, 134, 172, 196, 242, 292, 346};
    for (int v = 1; v <= 10; ++v) {
        EXPECT_EQ(totalCodewords(v), total[v - 1]) << "version " << v;
        for (int e = 0; e < 4; ++e) EXPECT_EQ(dataCodewords(v, static_cast<Ecc>(e)), cap[e][v - 1]) << "v" << v << " e" << e;
    }
}

TEST(QrEncode, ModeSelection) {
    EXPECT_EQ(bestMode("0123456789"), Mode::Numeric);
    EXPECT_EQ(bestMode("HELLO WORLD $%*+-./:"), Mode::Alphanumeric);
    EXPECT_EQ(bestMode("hello"), Mode::Byte);
    EXPECT_EQ(bestMode("doação"), Mode::Byte);
}

TEST(QrEncode, RejectsBadInput) {
    QrCode q;
    EXPECT_FALSE(encode("", Ecc::M, q));
    EXPECT_FALSE(encode(std::string(500, 'a'), Ecc::H, q));  // does not fit in version <= 10
    EXPECT_FALSE(encode("x", Ecc::M, q, 11, 12));            // range outside what we support
}

TEST(QrEncode, Version1NumericLayout) {
    QrCode q;
    ASSERT_TRUE(encode("01234567", Ecc::M, q, 1, 1, 0));
    EXPECT_EQ(q.version, 1);
    EXPECT_EQ(q.size, 21);
    EXPECT_EQ(q.mask, 0);
    // finder patterns, timing and dark module
    for (int i = 0; i < 7; ++i) { EXPECT_TRUE(q.get(i, 0)); EXPECT_TRUE(q.get(0, i)); EXPECT_TRUE(q.get(20 - i, 0)); EXPECT_TRUE(q.get(i, 20)); }
    EXPECT_FALSE(q.get(1, 1));
    EXPECT_TRUE(q.get(3, 3));
    EXPECT_TRUE(q.get(8, 6));
    EXPECT_FALSE(q.get(9, 6));
    EXPECT_TRUE(q.get(8, 13));  // dark module at (8, 4*version + 9)
    const auto d = readBack(q);
    ASSERT_TRUE(d.ok);
    EXPECT_EQ(d.text, "01234567");
    EXPECT_EQ(d.mask, 0);
    EXPECT_EQ(d.ecc, Ecc::M);
}

TEST(QrEncode, RoundTripAllLevelsAndModes) {
    const std::vector<std::string> texts = {
        "01234567890123456789", "HELLO WORLD", "https://example.com/doacao?id=1",
        "Ol\xC3\xA1, doa\xC3\xA7\xC3\xA3o!",  // UTF-8 bytes
        "00020126580014BR.GOV.BCB.PIX0136123e4567-e89b-12d3-a456-4266141740005204000053039865802BR5913Fulano de Tal6008BRASILIA62070503***63041D3D"};
    for (const auto& t : texts)
        for (int e = 0; e < 4; ++e) {
            QrCode q;
            if (!encode(t, static_cast<Ecc>(e), q)) { EXPECT_GT(t.size(), 100u) << t; continue; }
            const auto d = readBack(q);
            ASSERT_TRUE(d.ok) << t << " ecc " << e << " v" << q.version;
            EXPECT_EQ(d.text, t);
            EXPECT_EQ(static_cast<int>(d.ecc), e);
            EXPECT_EQ(d.mask, q.mask);
        }
}

TEST(QrEncode, RoundTripEveryVersionAndMask) {
    for (int v = 1; v <= 10; ++v)
        for (int e = 0; e < 4; ++e) {
            const int cap = dataCodewords(v, static_cast<Ecc>(e));
            // byte-mode text that is exactly large enough to need this version
            const int chars = cap - (v <= 9 ? 2 : 3);
            std::string t;
            for (int i = 0; i < chars; ++i) t += static_cast<char>('a' + (i * 7) % 26);
            for (int mask : {-1, 0, 3, 7}) {
                QrCode q;
                ASSERT_TRUE(encode(t, static_cast<Ecc>(e), q, v, v, mask)) << "v" << v << " e" << e;
                EXPECT_EQ(q.version, v);
                const auto d = readBack(q);
                ASSERT_TRUE(d.ok) << "v" << v << " e" << e << " mask" << mask;
                EXPECT_EQ(d.text, t);
            }
        }
}

TEST(QrEncode, PicksSmallestVersionAndIsDeterministic) {
    QrCode a, b;
    ASSERT_TRUE(encode("HB STUDIO", Ecc::L, a));
    ASSERT_TRUE(encode("HB STUDIO", Ecc::L, b));
    EXPECT_EQ(a.version, 1);
    EXPECT_EQ(a.modules, b.modules);
    QrCode c;
    ASSERT_TRUE(encode(std::string(30, 'x'), Ecc::L, c));  // 30 bytes needs 2-L (cap 34 - 2)
    EXPECT_EQ(c.version, 2);
    QrCode out;
    EXPECT_FALSE(out.get(0, 0));
    EXPECT_FALSE(c.get(-1, 0));
    EXPECT_FALSE(c.get(0, 999));
}
