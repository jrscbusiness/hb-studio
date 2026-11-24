#include "qr/qr.hpp"

#include <algorithm>
#include <cstdlib>

namespace hb::qr {

namespace {

// Per version 1..10 (index 0 = version 1). Verified in tests against the spec's data-capacity table.
constexpr int kEccPerBlock[4][10] = {
    {7, 10, 15, 20, 26, 18, 20, 24, 30, 18},    // L
    {10, 16, 26, 18, 24, 16, 18, 22, 22, 26},   // M
    {13, 22, 18, 26, 18, 24, 18, 22, 20, 24},   // Q
    {17, 28, 22, 16, 22, 28, 26, 26, 24, 28}};  // H
constexpr int kBlocks[4][10] = {
    {1, 1, 1, 1, 1, 2, 2, 2, 2, 4},
    {1, 1, 1, 2, 2, 4, 4, 4, 5, 5},
    {1, 1, 2, 2, 4, 4, 6, 6, 8, 8},
    {1, 1, 2, 4, 4, 4, 5, 6, 8, 8}};

std::vector<int> alignmentPositions(int v) {
    switch (v) {
        case 1: return {};
        case 2: return {6, 18};
        case 3: return {6, 22};
        case 4: return {6, 26};
        case 5: return {6, 30};
        case 6: return {6, 34};
        case 7: return {6, 22, 38};
        case 8: return {6, 24, 42};
        case 9: return {6, 26, 46};
        default: return {6, 28, 50};
    }
}

int eccBits(Ecc e) {  // 2-bit indicator used in the format information
    switch (e) {
        case Ecc::L: return 1;
        case Ecc::M: return 0;
        case Ecc::Q: return 3;
        default: return 2;
    }
}

bool inRange(int v) { return v >= kMinVersion && v <= kMaxVersion; }

int alnumValue(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
    switch (c) {
        case ' ': return 36; case '$': return 37; case '%': return 38; case '*': return 39; case '+': return 40;
        case '-': return 41; case '.': return 42; case '/': return 43; case ':': return 44;
        default: return -1;
    }
}

int countBits(Mode m, int version) {
    const bool small = version <= 9;
    switch (m) {
        case Mode::Numeric: return small ? 10 : 12;
        case Mode::Alphanumeric: return small ? 9 : 11;
        default: return small ? 8 : 16;
    }
}

struct BitBuffer {
    std::vector<std::uint8_t> bits;  // one bit per element, simple and fast enough for <= 3 kbit
    void append(std::uint32_t value, int count) {
        for (int i = count - 1; i >= 0; --i) bits.push_back(static_cast<std::uint8_t>((value >> i) & 1));
    }
};

bool buildDataBits(std::string_view text, Mode mode, int version, BitBuffer& bb) {
    const int cb = countBits(mode, version);
    if (text.size() >= (1u << cb)) return false;
    const auto n = static_cast<std::uint32_t>(text.size());
    switch (mode) {
        case Mode::Numeric:
            bb.append(1, 4);
            bb.append(n, cb);
            for (std::size_t i = 0; i < text.size();) {
                const std::size_t k = std::min<std::size_t>(3, text.size() - i);
                std::uint32_t v = 0;
                for (std::size_t j = 0; j < k; ++j) v = v * 10 + static_cast<std::uint32_t>(text[i + j] - '0');
                bb.append(v, static_cast<int>(k) * 3 + 1);
                i += k;
            }
            break;
        case Mode::Alphanumeric:
            bb.append(2, 4);
            bb.append(n, cb);
            for (std::size_t i = 0; i < text.size(); i += 2) {
                if (i + 1 < text.size())
                    bb.append(static_cast<std::uint32_t>(alnumValue(static_cast<unsigned char>(text[i])) * 45 +
                                                         alnumValue(static_cast<unsigned char>(text[i + 1]))), 11);
                else
                    bb.append(static_cast<std::uint32_t>(alnumValue(static_cast<unsigned char>(text[i]))), 6);
            }
            break;
        case Mode::Byte:
            bb.append(4, 4);
            bb.append(n, cb);
            for (char c : text) bb.append(static_cast<unsigned char>(c), 8);
            break;
    }
    return true;
}

std::vector<std::uint8_t> padToCodewords(BitBuffer& bb, int capacityBytes) {
    const std::size_t capBits = static_cast<std::size_t>(capacityBytes) * 8;
    bb.append(0, static_cast<int>(std::min<std::size_t>(4, capBits - bb.bits.size())));  // terminator
    while (bb.bits.size() % 8) bb.bits.push_back(0);
    std::vector<std::uint8_t> out(bb.bits.size() / 8, 0);
    for (std::size_t i = 0; i < bb.bits.size(); ++i) out[i >> 3] = static_cast<std::uint8_t>(out[i >> 3] | (bb.bits[i] << (7 - (i & 7))));
    for (std::uint8_t pad = 0xEC; out.size() < static_cast<std::size_t>(capacityBytes); pad ^= 0xEC ^ 0x11) out.push_back(pad);
    return out;
}

// ---- matrix construction ----
struct Matrix {
    int size;
    std::vector<std::uint8_t> mod, func;
    explicit Matrix(int s) : size(s), mod(static_cast<std::size_t>(s) * s, 0), func(static_cast<std::size_t>(s) * s, 0) {}
    std::size_t at(int x, int y) const { return static_cast<std::size_t>(y) * size + x; }
    void setFunc(int x, int y, bool dark) {
        if (x < 0 || y < 0 || x >= size || y >= size) return;
        mod[at(x, y)] = dark; func[at(x, y)] = 1;
    }
};

bool bitAt(std::uint32_t v, int i) { return ((v >> i) & 1) != 0; }

void drawFormat(Matrix& m, Ecc ecc, int mask) {
    const std::uint32_t bits = formatBits(ecc, mask);
    const int n = m.size;
    for (int i = 0; i <= 5; ++i) m.setFunc(8, i, bitAt(bits, i));
    m.setFunc(8, 7, bitAt(bits, 6));
    m.setFunc(8, 8, bitAt(bits, 7));
    m.setFunc(7, 8, bitAt(bits, 8));
    for (int i = 9; i < 15; ++i) m.setFunc(14 - i, 8, bitAt(bits, i));
    for (int i = 0; i < 8; ++i) m.setFunc(n - 1 - i, 8, bitAt(bits, i));
    for (int i = 8; i < 15; ++i) m.setFunc(8, n - 15 + i, bitAt(bits, i));
    m.setFunc(8, n - 8, true);  // the always-dark module
}

void drawFunctionPatterns(Matrix& m, int version, Ecc ecc) {
    const int n = m.size;
    for (int i = 0; i < n; ++i) { m.setFunc(6, i, i % 2 == 0); m.setFunc(i, 6, i % 2 == 0); }
    auto finder = [&](int cx, int cy) {
        for (int dy = -4; dy <= 4; ++dy)
            for (int dx = -4; dx <= 4; ++dx) {
                const int d = std::max(std::abs(dx), std::abs(dy));
                m.setFunc(cx + dx, cy + dy, d != 2 && d != 4);
            }
    };
    finder(3, 3); finder(n - 4, 3); finder(3, n - 4);
    const auto pos = alignmentPositions(version);
    for (std::size_t i = 0; i < pos.size(); ++i)
        for (std::size_t j = 0; j < pos.size(); ++j) {
            const bool corner = (i == 0 && j == 0) || (i == 0 && j + 1 == pos.size()) || (i + 1 == pos.size() && j == 0);
            if (corner) continue;
            for (int dy = -2; dy <= 2; ++dy)
                for (int dx = -2; dx <= 2; ++dx) m.setFunc(pos[i] + dx, pos[j] + dy, std::max(std::abs(dx), std::abs(dy)) != 1);
        }
    drawFormat(m, ecc, 0);  // reserve the area; rewritten with the real mask later
    if (version >= 7) {
        const std::uint32_t bits = versionBits(version);
        for (int i = 0; i < 18; ++i) {
            const int a = n - 11 + i % 3, b = i / 3;
            m.setFunc(a, b, bitAt(bits, i)); m.setFunc(b, a, bitAt(bits, i));
        }
    }
}

void placeCodewords(Matrix& m, const std::vector<std::uint8_t>& cw) {
    std::size_t i = 0;
    const std::size_t total = cw.size() * 8;
    for (int right = m.size - 1; right >= 1; right -= 2) {
        if (right == 6) right = 5;
        for (int vert = 0; vert < m.size; ++vert)
            for (int j = 0; j < 2; ++j) {
                const int x = right - j;
                const bool upward = ((right + 1) & 2) == 0;
                const int y = upward ? m.size - 1 - vert : vert;
                if (!m.func[m.at(x, y)] && i < total) {
                    m.mod[m.at(x, y)] = static_cast<std::uint8_t>((cw[i >> 3] >> (7 - (i & 7))) & 1);
                    ++i;
                }
            }
    }
}

bool maskBit(int mask, int x, int y) {
    switch (mask) {
        case 0: return (x + y) % 2 == 0;
        case 1: return y % 2 == 0;
        case 2: return x % 3 == 0;
        case 3: return (x + y) % 3 == 0;
        case 4: return (x / 3 + y / 2) % 2 == 0;
        case 5: return x * y % 2 + x * y % 3 == 0;
        case 6: return (x * y % 2 + x * y % 3) % 2 == 0;
        default: return ((x + y) % 2 + x * y % 3) % 2 == 0;
    }
}

void applyMask(Matrix& m, int mask) {  // XOR: applying twice restores
    for (int y = 0; y < m.size; ++y)
        for (int x = 0; x < m.size; ++x)
            if (!m.func[m.at(x, y)] && maskBit(mask, x, y)) m.mod[m.at(x, y)] ^= 1;
}

int penalty(const Matrix& m) {
    const int n = m.size;
    auto dark = [&](int x, int y) { return m.mod[m.at(x, y)] != 0; };
    int p = 0;
    for (int pass = 0; pass < 2; ++pass) {  // N1 (runs) and N3 (finder-like), rows then columns
        for (int a = 0; a < n; ++a) {
            auto cell = [&](int b) { return pass == 0 ? dark(b, a) : dark(a, b); };
            int run = 1;
            for (int b = 1; b <= n; ++b) {
                if (b < n && cell(b) == cell(b - 1)) { ++run; continue; }
                if (run >= 5) p += 3 + (run - 5);
                run = 1;
            }
            static const int pat1[11] = {1, 0, 1, 1, 1, 0, 1, 0, 0, 0, 0};
            static const int pat2[11] = {0, 0, 0, 0, 1, 0, 1, 1, 1, 0, 1};
            for (int b = 0; b + 11 <= n; ++b) {
                bool m1 = true, m2 = true;
                for (int k = 0; k < 11; ++k) {
                    const int v = cell(b + k) ? 1 : 0;
                    if (v != pat1[k]) m1 = false;
                    if (v != pat2[k]) m2 = false;
                }
                if (m1) p += 40;
                if (m2) p += 40;
            }
        }
    }
    for (int y = 0; y + 1 < n; ++y)  // N2: 2x2 blocks
        for (int x = 0; x + 1 < n; ++x) {
            const bool c = dark(x, y);
            if (c == dark(x + 1, y) && c == dark(x, y + 1) && c == dark(x + 1, y + 1)) p += 3;
        }
    int darkCount = 0;
    for (auto v : m.mod) darkCount += v ? 1 : 0;
    const int total = n * n;
    const int k = (std::abs(darkCount * 20 - total * 10) + total - 1) / total - 1;  // N4
    p += std::max(0, k) * 10;
    return p;
}

}  // namespace

// ---- public building blocks ----
std::uint8_t gfMul(std::uint8_t a, std::uint8_t b) {
    unsigned z = 0;
    for (int i = 7; i >= 0; --i) {
        z = (z << 1) ^ ((z >> 7) * 0x11D);
        z ^= ((b >> i) & 1u) * a;
    }
    return static_cast<std::uint8_t>(z);
}

std::vector<std::uint8_t> rsDivisor(int degree) {
    std::vector<std::uint8_t> r(static_cast<std::size_t>(std::max(degree, 0)), 0);
    if (degree <= 0) return r;
    r.back() = 1;
    std::uint8_t root = 1;
    for (int i = 0; i < degree; ++i) {
        for (std::size_t j = 0; j < r.size(); ++j) {
            r[j] = gfMul(r[j], root);
            if (j + 1 < r.size()) r[j] ^= r[j + 1];
        }
        root = gfMul(root, 2);
    }
    return r;
}

std::vector<std::uint8_t> rsRemainder(const std::vector<std::uint8_t>& data, const std::vector<std::uint8_t>& divisor) {
    std::vector<std::uint8_t> r(divisor.size(), 0);
    if (divisor.empty()) return r;
    for (std::uint8_t b : data) {
        const std::uint8_t factor = b ^ r.front();
        r.erase(r.begin());
        r.push_back(0);
        for (std::size_t i = 0; i < r.size(); ++i) r[i] ^= gfMul(divisor[i], factor);
    }
    return r;
}

std::uint32_t formatBits(Ecc ecc, int mask) {
    const std::uint32_t data = static_cast<std::uint32_t>(eccBits(ecc) << 3 | (mask & 7));
    std::uint32_t rem = data;
    for (int i = 0; i < 10; ++i) rem = (rem << 1) ^ ((rem >> 9) * 0x537);
    return ((data << 10) | rem) ^ 0x5412;
}

std::uint32_t versionBits(int version) {
    std::uint32_t rem = static_cast<std::uint32_t>(version);
    for (int i = 0; i < 12; ++i) rem = (rem << 1) ^ ((rem >> 11) * 0x1F25);
    return (static_cast<std::uint32_t>(version) << 12) | rem;
}

int totalCodewords(int version) {
    if (!inRange(version)) return 0;
    int r = (16 * version + 128) * version + 64;
    if (version >= 2) {
        const int na = version / 7 + 2;
        r -= (25 * na - 10) * na - 55;
        if (version >= 7) r -= 36;
    }
    return r / 8;
}

int eccBlocks(int version, Ecc ecc) { return inRange(version) ? kBlocks[static_cast<int>(ecc)][version - 1] : 0; }

int dataCodewords(int version, Ecc ecc) {
    if (!inRange(version)) return 0;
    return totalCodewords(version) - kEccPerBlock[static_cast<int>(ecc)][version - 1] * eccBlocks(version, ecc);
}

std::vector<std::uint8_t> addEccAndInterleave(const std::vector<std::uint8_t>& data, int version, Ecc ecc) {
    if (!inRange(version) || static_cast<int>(data.size()) != dataCodewords(version, ecc)) return {};
    const int blocks = eccBlocks(version, ecc);
    const int eccLen = kEccPerBlock[static_cast<int>(ecc)][version - 1];
    const int total = totalCodewords(version);
    const int numShort = blocks - total % blocks;
    const int shortLen = total / blocks;
    const auto divisor = rsDivisor(eccLen);
    std::vector<std::vector<std::uint8_t>> dataBlocks, eccBlocksV;
    std::size_t k = 0;
    for (int i = 0; i < blocks; ++i) {
        const int len = shortLen - eccLen + (i < numShort ? 0 : 1);
        std::vector<std::uint8_t> blk(data.begin() + static_cast<std::ptrdiff_t>(k), data.begin() + static_cast<std::ptrdiff_t>(k) + len);
        k += static_cast<std::size_t>(len);
        eccBlocksV.push_back(rsRemainder(blk, divisor));
        dataBlocks.push_back(std::move(blk));
    }
    std::vector<std::uint8_t> out;
    for (int i = 0; i <= shortLen - eccLen; ++i)
        for (int b = 0; b < blocks; ++b)
            if (i < static_cast<int>(dataBlocks[static_cast<std::size_t>(b)].size())) out.push_back(dataBlocks[static_cast<std::size_t>(b)][static_cast<std::size_t>(i)]);
    for (int i = 0; i < eccLen; ++i)
        for (int b = 0; b < blocks; ++b) out.push_back(eccBlocksV[static_cast<std::size_t>(b)][static_cast<std::size_t>(i)]);
    return out;
}

Mode bestMode(std::string_view text) {
    bool numeric = true, alnum = true;
    for (char ch : text) {
        const auto c = static_cast<unsigned char>(ch);
        if (c < '0' || c > '9') numeric = false;
        if (alnumValue(c) < 0) alnum = false;
    }
    return numeric ? Mode::Numeric : (alnum ? Mode::Alphanumeric : Mode::Byte);
}

bool encode(std::string_view text, Ecc ecc, QrCode& out, int minVersion, int maxVersion, int forcedMask) {
    if (text.empty() || text.size() > 4000) return false;
    minVersion = std::max(minVersion, kMinVersion);
    maxVersion = std::min(maxVersion, kMaxVersion);
    const Mode mode = bestMode(text);
    for (int v = minVersion; v <= maxVersion; ++v) {
        BitBuffer bb;
        if (!buildDataBits(text, mode, v, bb)) continue;
        const int cap = dataCodewords(v, ecc);
        if (bb.bits.size() > static_cast<std::size_t>(cap) * 8) continue;
        const auto data = padToCodewords(bb, cap);
        const auto cw = addEccAndInterleave(data, v, ecc);
        if (cw.empty()) return false;

        Matrix m(17 + 4 * v);
        drawFunctionPatterns(m, v, ecc);
        placeCodewords(m, cw);
        int best = forcedMask >= 0 && forcedMask < 8 ? forcedMask : 0;
        if (!(forcedMask >= 0 && forcedMask < 8)) {
            int bestPen = 1 << 30;
            for (int mk = 0; mk < 8; ++mk) {
                applyMask(m, mk);
                drawFormat(m, ecc, mk);
                const int p = penalty(m);
                if (p < bestPen) { bestPen = p; best = mk; }
                applyMask(m, mk);
            }
        }
        applyMask(m, best);
        drawFormat(m, ecc, best);
        out.version = v;
        out.size = m.size;
        out.ecc = ecc;
        out.mask = best;
        out.modules = std::move(m.mod);
        return true;
    }
    return false;
}

}  // namespace hb::qr
