#pragma once
// Test-only independent readers for the formats the recording module writes: RIFF/WAV, FLAC, AVI,
// baseline JPEG. They are written from the specifications and do NOT share code with the writers (apart
// from MD5, which has its own RFC 1321 test vectors).
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "video/program_output.hpp"

namespace hb::rectest {

inline std::vector<std::uint8_t> readFile(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

struct TempDir {
    std::filesystem::path path;
    explicit TempDir(const std::string& tag) {
        static int counter = 0;
        path = std::filesystem::temp_directory_path() /
               ("hbrec_" + tag + "_" + std::to_string(static_cast<long long>(std::chrono::steady_clock::now().time_since_epoch().count())) + "_" + std::to_string(counter++));
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    std::string utf8(const std::string& leaf) const {
        const auto u = (path / std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(leaf.data()), leaf.size()))).u8string();
        return std::string(reinterpret_cast<const char*>(u.data()), u.size());
    }
    std::string folderUtf8() const {
        const auto u = path.u8string();
        return std::string(reinterpret_cast<const char*>(u.data()), u.size());
    }
};

inline std::uint32_t le32(const std::uint8_t* p) {
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) | (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
}
inline std::uint32_t le16(const std::uint8_t* p) { return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8); }

// ------------------------------------------------------------------ WAV
struct WavInfo {
    bool ok = false;
    std::string error;
    int formatTag = 0, channels = 0, sampleRate = 0, bits = 0, validBits = 0;
    std::uint32_t channelMask = 0;
    std::uint32_t subFormat = 0;  // first 16 bits of the GUID
    std::uint64_t frames = 0;
    std::vector<std::int32_t> ints;  // PCM samples (interleaved), sign-extended
    std::vector<float> floats;       // when IEEE float
    std::uint32_t riffSize = 0, dataSize = 0;
    bool hasFact = false;
    std::uint64_t fileSize = 0;
};

inline WavInfo parseWav(const std::vector<std::uint8_t>& d) {
    WavInfo w;
    w.fileSize = d.size();
    const auto fail = [&](const char* m) {
        w.error = m;
        return w;
    };
    if (d.size() < 12 || std::memcmp(d.data(), "RIFF", 4) || std::memcmp(d.data() + 8, "WAVE", 4)) return fail("no RIFF/WAVE");
    w.riffSize = le32(&d[4]);
    if (w.riffSize + 8 != d.size()) return fail("RIFF size mismatch");
    std::size_t pos = 12;
    bool gotFmt = false, gotData = false;
    while (pos + 8 <= d.size()) {
        const std::uint32_t sz = le32(&d[pos + 4]);
        const std::uint8_t* body = &d[pos + 8];
        if (pos + 8 + sz > d.size()) return fail("chunk past EOF");
        if (!std::memcmp(&d[pos], "fmt ", 4)) {
            if (sz < 16) return fail("short fmt");
            w.formatTag = static_cast<int>(le16(body));
            w.channels = static_cast<int>(le16(body + 2));
            w.sampleRate = static_cast<int>(le32(body + 4));
            const int blockAlign = static_cast<int>(le16(body + 12));
            w.bits = static_cast<int>(le16(body + 14));
            if (static_cast<int>(le32(body + 8)) != w.sampleRate * blockAlign) return fail("byte rate");
            if (blockAlign != w.channels * w.bits / 8) return fail("block align");
            w.validBits = w.bits;
            if (w.formatTag == 0xFFFE) {
                if (sz < 40 || le16(body + 16) != 22) return fail("bad extensible");
                w.validBits = static_cast<int>(le16(body + 18));
                w.channelMask = le32(body + 20);
                w.subFormat = le16(body + 24);
                static const std::uint8_t tail[] = {0, 0, 0x10, 0, 0x80, 0, 0, 0xAA, 0, 0x38, 0x9B, 0x71};
                if (std::memcmp(body + 28, tail, 12)) return fail("bad GUID");
            } else {
                w.subFormat = static_cast<std::uint32_t>(w.formatTag);
            }
            gotFmt = true;
        } else if (!std::memcmp(&d[pos], "fact", 4)) {
            w.hasFact = true;
        } else if (!std::memcmp(&d[pos], "data", 4)) {
            if (!gotFmt) return fail("data before fmt");
            w.dataSize = sz;
            const int bytes = w.bits / 8;
            if (sz % (static_cast<std::uint32_t>(w.channels) * bytes)) return fail("partial frame");
            w.frames = sz / (static_cast<std::uint32_t>(w.channels) * bytes);
            const std::size_t n = sz / bytes;
            if (w.subFormat == 3) {
                w.floats.resize(n);
                for (std::size_t i = 0; i < n; ++i) {
                    std::uint32_t u = le32(body + i * 4);
                    std::memcpy(&w.floats[i], &u, 4);
                }
            } else {
                w.ints.resize(n);
                for (std::size_t i = 0; i < n; ++i) {
                    const std::uint8_t* p = body + i * bytes;
                    std::int32_t v;
                    if (bytes == 2) v = static_cast<std::int16_t>(le16(p));
                    else v = static_cast<std::int32_t>((std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) | (std::uint32_t(p[2]) << 16)) << 8) >> 8;
                    w.ints[i] = v;
                }
            }
            gotData = true;
        }
        pos += 8 + sz + (sz & 1);
    }
    if (pos != d.size()) return fail("trailing bytes");
    if (!gotFmt || !gotData) return fail("missing fmt/data");
    w.ok = true;
    return w;
}

// ------------------------------------------------------------------ FLAC
struct FlacInfo {
    bool ok = false;
    std::string error;
    int minBlock = 0, maxBlock = 0, minFrame = 0, maxFrame = 0, sampleRate = 0, channels = 0, bps = 0;
    std::uint64_t totalSamples = 0;
    std::uint8_t md5[16] = {};
    std::vector<std::int32_t> samples;  // interleaved
    int frames = 0;
    int fixedSubframes = 0, constantSubframes = 0, verbatimSubframes = 0;
    int stereoModes[4] = {0, 0, 0, 0};  // independent, left/side, side/right, mid/side
};

namespace detail_flac {
struct Bits {
    const std::vector<std::uint8_t>& d;
    std::size_t bytePos;
    int bit = 0;  // bits consumed in current byte
    bool bad = false;
    Bits(const std::vector<std::uint8_t>& data, std::size_t pos) : d(data), bytePos(pos) {}
    std::uint32_t read(int n) {  // n <= 32
        std::uint64_t v = 0;
        for (int i = 0; i < n; ++i) {
            if (bytePos >= d.size()) {
                bad = true;
                return 0;
            }
            v = (v << 1) | ((d[bytePos] >> (7 - bit)) & 1);
            if (++bit == 8) {
                bit = 0;
                ++bytePos;
            }
        }
        return static_cast<std::uint32_t>(v);
    }
    std::int32_t readSigned(int n) {
        if (n == 0) return 0;
        const std::uint32_t v = read(n);
        const std::uint32_t sign = 1u << (n - 1);
        return static_cast<std::int32_t>((v ^ sign) - sign);
    }
    std::uint32_t unary() {
        std::uint32_t q = 0;
        while (!bad && read(1) == 0) ++q;
        return q;
    }
    void align() {
        if (bit) {
            bit = 0;
            ++bytePos;
        }
    }
};
inline std::uint8_t crc8(const std::uint8_t* p, std::size_t n) {
    std::uint8_t c = 0;
    for (std::size_t i = 0; i < n; ++i) {
        c ^= p[i];
        for (int k = 0; k < 8; ++k) c = (c & 0x80) ? static_cast<std::uint8_t>((c << 1) ^ 0x07) : static_cast<std::uint8_t>(c << 1);
    }
    return c;
}
inline std::uint16_t crc16(const std::uint8_t* p, std::size_t n) {
    std::uint16_t c = 0;
    for (std::size_t i = 0; i < n; ++i) {
        c ^= static_cast<std::uint16_t>(p[i] << 8);
        for (int k = 0; k < 8; ++k) c = (c & 0x8000) ? static_cast<std::uint16_t>((c << 1) ^ 0x8005) : static_cast<std::uint16_t>(c << 1);
    }
    return c;
}
}  // namespace detail_flac

inline FlacInfo parseFlac(const std::vector<std::uint8_t>& d) {
    using namespace detail_flac;
    FlacInfo f;
    const auto fail = [&](const std::string& m) {
        f.error = m;
        return f;
    };
    if (d.size() < 42 || std::memcmp(d.data(), "fLaC", 4)) return fail("no fLaC");
    std::size_t pos = 4;
    bool last = false, gotInfo = false;
    while (!last) {
        if (pos + 4 > d.size()) return fail("truncated metadata");
        last = d[pos] & 0x80;
        const int type = d[pos] & 0x7F;
        const std::size_t len = (std::size_t(d[pos + 1]) << 16) | (std::size_t(d[pos + 2]) << 8) | d[pos + 3];
        pos += 4;
        if (pos + len > d.size()) return fail("metadata past EOF");
        if (type == 0) {
            if (len != 34) return fail("STREAMINFO length");
            Bits b(d, pos);
            f.minBlock = static_cast<int>(b.read(16));
            f.maxBlock = static_cast<int>(b.read(16));
            f.minFrame = static_cast<int>(b.read(24));
            f.maxFrame = static_cast<int>(b.read(24));
            f.sampleRate = static_cast<int>(b.read(20));
            f.channels = static_cast<int>(b.read(3)) + 1;
            f.bps = static_cast<int>(b.read(5)) + 1;
            f.totalSamples = (std::uint64_t(b.read(4)) << 32) | b.read(32);
            for (int i = 0; i < 16; ++i) f.md5[i] = static_cast<std::uint8_t>(b.read(8));
            gotInfo = true;
        }
        pos += len;
    }
    if (!gotInfo) return fail("no STREAMINFO");
    std::vector<std::vector<std::int32_t>> ch(static_cast<std::size_t>(f.channels));
    std::uint64_t expectFrameNo = 0;
    while (pos < d.size()) {
        const std::size_t start = pos;
        Bits b(d, pos);
        if (b.read(14) != 0x3FFE) return fail("bad sync at frame " + std::to_string(expectFrameNo));
        b.read(1);
        if (b.read(1) != 0) return fail("variable block size not expected");
        const int bsCode = static_cast<int>(b.read(4));
        const int srCode = static_cast<int>(b.read(4));
        const int chAssign = static_cast<int>(b.read(4));
        const int ssCode = static_cast<int>(b.read(3));
        b.read(1);
        // UTF-8 coded frame number
        const std::uint32_t lead = b.read(8);
        std::uint64_t num;
        int extra = 0;
        if (lead < 0x80) num = lead;
        else {
            while (lead & (0x80 >> (extra + 1))) ++extra;
            num = lead & (0x7F >> (extra + 1));
            for (int i = 0; i < extra; ++i) {
                const std::uint32_t c = b.read(8);
                if ((c & 0xC0) != 0x80) return fail("bad utf8 number");
                num = (num << 6) | (c & 0x3F);
            }
        }
        if (num != expectFrameNo) return fail("frame number " + std::to_string(num));
        ++expectFrameNo;
        int blockSize = 0;
        if (bsCode == 12) blockSize = 4096;
        else if (bsCode == 7) blockSize = static_cast<int>(b.read(16)) + 1;
        else if (bsCode == 6) blockSize = static_cast<int>(b.read(8)) + 1;
        else return fail("unsupported block size code");
        if (srCode != 0) return fail("expected rate from STREAMINFO");
        int bps = f.bps;
        const int hdrBps = ssCode == 4 ? 16 : ssCode == 6 ? 24 : 0;
        if (hdrBps != f.bps) return fail("sample size code");
        const std::size_t hdrEnd = b.bytePos;
        const std::uint32_t c8 = b.read(8);
        if (b.bad || crc8(&d[start], hdrEnd - start) != c8) return fail("CRC-8 mismatch");
        int nch = f.channels;
        if (chAssign < 8) {
            if (chAssign + 1 != f.channels) return fail("channel assignment");
            f.stereoModes[0]++;
        } else if (chAssign <= 10) {
            if (f.channels != 2) return fail("stereo mode on non-stereo");
            f.stereoModes[chAssign - 7]++;
        } else return fail("reserved channel assignment");
        std::vector<std::vector<std::int32_t>> sub(static_cast<std::size_t>(nch), std::vector<std::int32_t>(static_cast<std::size_t>(blockSize)));
        for (int c = 0; c < nch; ++c) {
            int sbps = bps;
            if ((chAssign == 8 && c == 1) || (chAssign == 9 && c == 0) || (chAssign == 10 && c == 1)) ++sbps;
            auto& s = sub[static_cast<std::size_t>(c)];
            if (b.read(1) != 0) return fail("subframe pad bit");
            const int type = static_cast<int>(b.read(6));
            if (b.read(1) != 0) return fail("wasted bits unsupported");
            if (type == 0) {
                const std::int32_t v = b.readSigned(sbps);
                std::fill(s.begin(), s.end(), v);
                f.constantSubframes++;
            } else if (type == 1) {
                for (auto& x : s) x = b.readSigned(sbps);
                f.verbatimSubframes++;
            } else if (type >= 8 && type <= 12) {
                f.fixedSubframes++;
                const int order = type - 8;
                for (int i = 0; i < order; ++i) s[static_cast<std::size_t>(i)] = b.readSigned(sbps);
                const int method = static_cast<int>(b.read(2));
                if (method > 1) return fail("residual method");
                const int po = static_cast<int>(b.read(4));
                const int parts = 1 << po;
                if (blockSize % parts) return fail("partition order");
                const int ps = blockSize / parts;
                if (ps <= order) return fail("partition too small");
                for (int j = 0; j < parts; ++j) {
                    const int k = static_cast<int>(b.read(method ? 5 : 4));
                    if (k == (method ? 31 : 15)) return fail("escape partition unsupported");
                    const int begin = j == 0 ? order : j * ps;
                    for (int i = begin; i < (j + 1) * ps; ++i) {
                        const std::uint32_t q = b.unary();
                        const std::uint64_t u = (std::uint64_t(q) << k) | (k ? b.read(k) : 0);
                        const std::int64_t r = static_cast<std::int64_t>(u >> 1) ^ -static_cast<std::int64_t>(u & 1);
                        const auto at = [&](int back) { return static_cast<std::int64_t>(s[static_cast<std::size_t>(i - back)]); };
                        std::int64_t pred = 0;
                        switch (order) {
                            case 1: pred = at(1); break;
                            case 2: pred = 2 * at(1) - at(2); break;
                            case 3: pred = 3 * at(1) - 3 * at(2) + at(3); break;
                            case 4: pred = 4 * at(1) - 6 * at(2) + 4 * at(3) - at(4); break;
                            default: break;
                        }
                        s[static_cast<std::size_t>(i)] = static_cast<std::int32_t>(pred + r);
                    }
                }
            } else {
                return fail("unsupported subframe type " + std::to_string(type));
            }
            if (b.bad) return fail("truncated frame");
        }
        b.align();
        const std::size_t crcPos = b.bytePos;
        const std::uint32_t c16 = b.read(16);
        if (b.bad || crc16(&d[start], crcPos - start) != c16) return fail("CRC-16 mismatch");
        pos = b.bytePos;
        if (chAssign == 8) {  // left/side
            for (int i = 0; i < blockSize; ++i) sub[1][static_cast<std::size_t>(i)] = sub[0][static_cast<std::size_t>(i)] - sub[1][static_cast<std::size_t>(i)];
        } else if (chAssign == 9) {  // side/right
            for (int i = 0; i < blockSize; ++i) sub[0][static_cast<std::size_t>(i)] += sub[1][static_cast<std::size_t>(i)];
        } else if (chAssign == 10) {  // mid/side
            for (int i = 0; i < blockSize; ++i) {
                const std::size_t k = static_cast<std::size_t>(i);
                std::int32_t mid = sub[0][k], side = sub[1][k];
                mid = static_cast<std::int32_t>((static_cast<std::uint32_t>(mid) << 1) | (static_cast<std::uint32_t>(side) & 1));
                sub[0][k] = (mid + side) >> 1;
                sub[1][k] = (mid - side) >> 1;
            }
        }
        for (int c = 0; c < nch; ++c) ch[static_cast<std::size_t>(c)].insert(ch[static_cast<std::size_t>(c)].end(), sub[static_cast<std::size_t>(c)].begin(), sub[static_cast<std::size_t>(c)].end());
        ++f.frames;
    }
    const std::size_t n = ch[0].size();
    f.samples.resize(n * static_cast<std::size_t>(f.channels));
    for (std::size_t i = 0; i < n; ++i)
        for (int c = 0; c < f.channels; ++c) f.samples[i * static_cast<std::size_t>(f.channels) + static_cast<std::size_t>(c)] = ch[static_cast<std::size_t>(c)][i];
    f.ok = true;
    return f;
}

// ------------------------------------------------------------------ AVI
struct AviChunk {
    std::string id;
    std::uint32_t offset, size;
    std::uint32_t flags;
    std::size_t dataPos;  // absolute file position of the payload
};
struct AviInfo {
    bool ok = false;
    std::string error;
    std::uint32_t usPerFrame = 0, totalFrames = 0, streams = 0, width = 0, height = 0, flags = 0;
    std::string vHandler, vCompression;
    std::uint32_t vScale = 0, vRate = 0, vLength = 0;
    std::uint32_t aScale = 0, aRate = 0, aLength = 0, aSampleSize = 0;
    int aChannels = 0, aSampleRate = 0, aBits = 0, aTag = 0;
    std::vector<AviChunk> chunks;       // from movi in file order
    std::vector<AviChunk> index;        // from idx1
    std::vector<std::vector<std::uint8_t>> videoFrames;
    std::vector<std::int16_t> audio;    // interleaved
    std::uint64_t audioFrames = 0;
};

inline AviInfo parseAvi(const std::vector<std::uint8_t>& d) {
    AviInfo a;
    const auto fail = [&](const std::string& m) {
        a.error = m;
        return a;
    };
    if (d.size() < 12 || std::memcmp(d.data(), "RIFF", 4) || std::memcmp(d.data() + 8, "AVI ", 4)) return fail("no RIFF AVI");
    if (le32(&d[4]) + 8 != d.size()) return fail("RIFF size mismatch");
    std::size_t moviFourcc = 0;
    bool gotMovi = false, gotIdx = false, gotHdrl = false;
    std::size_t pos = 12;
    int streamIdx = -1;
    while (pos + 8 <= d.size()) {
        const std::uint32_t sz = le32(&d[pos + 4]);
        if (pos + 8 + sz > d.size()) return fail("chunk past EOF");
        if (!std::memcmp(&d[pos], "LIST", 4)) {
            const std::size_t body = pos + 8;
            if (!std::memcmp(&d[body], "hdrl", 4)) {
                gotHdrl = true;
                std::size_t p = body + 4;
                const std::size_t end = body + sz;
                while (p + 8 <= end) {
                    const std::uint32_t s2 = le32(&d[p + 4]);
                    if (p + 8 + s2 > end) return fail("hdrl chunk overflow");
                    if (!std::memcmp(&d[p], "avih", 4)) {
                        if (s2 != 56) return fail("avih size");
                        const std::uint8_t* q = &d[p + 8];
                        a.usPerFrame = le32(q);
                        a.flags = le32(q + 12);
                        a.totalFrames = le32(q + 16);
                        a.streams = le32(q + 24);
                        a.width = le32(q + 32);
                        a.height = le32(q + 36);
                    } else if (!std::memcmp(&d[p], "LIST", 4) && !std::memcmp(&d[p + 8], "strl", 4)) {
                        ++streamIdx;
                        std::size_t r = p + 12;
                        const std::size_t rend = p + 8 + s2;
                        std::string type;
                        while (r + 8 <= rend) {
                            const std::uint32_t s3 = le32(&d[r + 4]);
                            if (r + 8 + s3 > rend) return fail("strl chunk overflow");
                            const std::uint8_t* q = &d[r + 8];
                            if (!std::memcmp(&d[r], "strh", 4)) {
                                if (s3 != 56) return fail("strh size");
                                type.assign(reinterpret_cast<const char*>(q), 4);
                                if (type == "vids") {
                                    a.vHandler.assign(reinterpret_cast<const char*>(q + 4), 4);
                                    a.vScale = le32(q + 20);
                                    a.vRate = le32(q + 24);
                                    a.vLength = le32(q + 32);
                                } else if (type == "auds") {
                                    a.aScale = le32(q + 20);
                                    a.aRate = le32(q + 24);
                                    a.aLength = le32(q + 32);
                                    a.aSampleSize = le32(q + 44);
                                }
                            } else if (!std::memcmp(&d[r], "strf", 4)) {
                                if (type == "vids") {
                                    if (le32(q) != 40) return fail("BITMAPINFOHEADER size");
                                    a.vCompression.assign(reinterpret_cast<const char*>(q + 16), 4);
                                    if (le32(q + 4) != a.width || le32(q + 8) != a.height) return fail("strf size mismatch");
                                } else if (type == "auds") {
                                    a.aTag = static_cast<int>(le16(q));
                                    a.aChannels = static_cast<int>(le16(q + 2));
                                    a.aSampleRate = static_cast<int>(le32(q + 4));
                                    a.aBits = static_cast<int>(le16(q + 14));
                                }
                            }
                            r += 8 + s3 + (s3 & 1);
                        }
                        if (r != rend) return fail("strl size mismatch");
                    }
                    p += 8 + s2 + (s2 & 1);
                }
                if (p != end) return fail("hdrl size mismatch");
            } else if (!std::memcmp(&d[body], "movi", 4)) {
                gotMovi = true;
                moviFourcc = body;
                std::size_t p = body + 4;
                const std::size_t end = body + sz;
                while (p + 8 <= end) {
                    const std::uint32_t s2 = le32(&d[p + 4]);
                    if (p + 8 + s2 > end) return fail("movi chunk overflow");
                    AviChunk c;
                    c.id.assign(reinterpret_cast<const char*>(&d[p]), 4);
                    c.size = s2;
                    c.offset = static_cast<std::uint32_t>(p - moviFourcc);
                    c.flags = 0;
                    c.dataPos = p + 8;
                    a.chunks.push_back(c);
                    p += 8 + s2 + (s2 & 1);
                }
                if (p != end) return fail("movi size mismatch");
            }
        } else if (!std::memcmp(&d[pos], "idx1", 4)) {
            gotIdx = true;
            if (sz % 16) return fail("idx1 size");
            for (std::uint32_t i = 0; i < sz / 16; ++i) {
                const std::uint8_t* q = &d[pos + 8 + i * 16];
                AviChunk c;
                c.id.assign(reinterpret_cast<const char*>(q), 4);
                c.flags = le32(q + 4);
                c.offset = le32(q + 8);
                c.size = le32(q + 12);
                c.dataPos = 0;
                a.index.push_back(c);
            }
        }
        pos += 8 + sz + (sz & 1);
    }
    if (pos != d.size()) return fail("trailing bytes");
    if (!gotHdrl || !gotMovi || !gotIdx) return fail("missing hdrl/movi/idx1");
    if (a.index.size() != a.chunks.size()) return fail("idx1 count mismatch");
    for (std::size_t i = 0; i < a.index.size(); ++i) {
        if (a.index[i].id != a.chunks[i].id || a.index[i].size != a.chunks[i].size || a.index[i].offset != a.chunks[i].offset)
            return fail("idx1 entry " + std::to_string(i) + " does not match the chunk");
    }
    for (const auto& c : a.chunks) {
        if (c.id == "00dc") a.videoFrames.emplace_back(d.begin() + static_cast<std::ptrdiff_t>(c.dataPos), d.begin() + static_cast<std::ptrdiff_t>(c.dataPos + c.size));
        else if (c.id == "01wb") {
            for (std::uint32_t i = 0; i + 1 < c.size; i += 2) a.audio.push_back(static_cast<std::int16_t>(le16(&d[c.dataPos + i])));
        } else return fail("unknown chunk " + c.id);
    }
    a.audioFrames = a.aChannels ? a.audio.size() / static_cast<std::size_t>(a.aChannels) : 0;
    a.ok = true;
    return a;
}

// ------------------------------------------------------------------ JPEG (baseline decoder)
struct DecodedImage {
    bool ok = false;
    std::string error;
    int width = 0, height = 0;
    int compSampling[3][2] = {{1, 1}, {1, 1}, {1, 1}};
    std::vector<std::uint8_t> rgb;  // width*height*3
};

namespace detail_jpeg {
struct Huff {
    int minCode[17], maxCode[18], valPtr[17];
    std::vector<std::uint8_t> vals;
    bool defined = false;
    void build(const std::uint8_t* counts, std::vector<std::uint8_t> v) {
        vals = std::move(v);
        int code = 0, k = 0;
        for (int l = 1; l <= 16; ++l) {
            valPtr[l] = k;
            minCode[l] = code;
            code += counts[l - 1];
            k += counts[l - 1];
            maxCode[l] = counts[l - 1] ? code - 1 : -1;
            code <<= 1;
        }
        defined = true;
    }
};
struct BitIn {
    const std::uint8_t* p;
    std::size_t n, pos;
    std::uint32_t acc = 0;
    int cnt = 0;
    bool bad = false;
    int bit() {
        if (cnt == 0) {
            if (pos >= n) {
                bad = true;
                return 0;
            }
            std::uint8_t b = p[pos++];
            if (b == 0xFF) {
                if (pos >= n) {
                    bad = true;
                    return 0;
                }
                const std::uint8_t b2 = p[pos++];
                if (b2 != 0x00) {
                    bad = true;  // marker inside entropy data
                    return 0;
                }
            }
            acc = b;
            cnt = 8;
        }
        --cnt;
        return (acc >> cnt) & 1;
    }
    int receive(int s) {
        int v = 0;
        for (int i = 0; i < s; ++i) v = (v << 1) | bit();
        return v;
    }
    static int extend(int v, int t) { return t && v < (1 << (t - 1)) ? v - (1 << t) + 1 : v; }
    int decode(const Huff& h) {
        int code = 0;
        for (int l = 1; l <= 16; ++l) {
            code = (code << 1) | bit();
            if (bad) return 0;
            if (h.maxCode[l] >= 0 && code <= h.maxCode[l] && code >= h.minCode[l]) return h.vals[static_cast<std::size_t>(h.valPtr[l] + code - h.minCode[l])];
        }
        bad = true;
        return 0;
    }
};
inline const int* zigzag() {
    static int z[64];
    static bool init = false;
    if (!init) {  // generated by walking the anti-diagonals
        int x = 0, y = 0;
        for (int i = 0; i < 64; ++i) {
            z[i] = y * 8 + x;
            if ((x + y) % 2 == 0) {
                if (x == 7) ++y;
                else if (y == 0) ++x;
                else {
                    ++x;
                    --y;
                }
            } else {
                if (y == 7) ++x;
                else if (x == 0) ++y;
                else {
                    --x;
                    ++y;
                }
            }
        }
        init = true;
    }
    return z;
}
inline void idct(const float* in, float* out) {
    static float c[8][8];
    static bool init = false;
    if (!init) {
        for (int x = 0; x < 8; ++x)
            for (int u = 0; u < 8; ++u) c[x][u] = static_cast<float>((u == 0 ? std::sqrt(0.5) : 1.0) * std::cos((2 * x + 1) * u * 3.14159265358979323846 / 16.0) / 2.0);
        init = true;
    }
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) {
            float s = 0;
            for (int v = 0; v < 8; ++v)
                for (int u = 0; u < 8; ++u) s += c[x][u] * c[y][v] * in[v * 8 + u];
            out[y * 8 + x] = s;
        }
}
}  // namespace detail_jpeg

inline DecodedImage decodeJpeg(const std::vector<std::uint8_t>& d) {
    using namespace detail_jpeg;
    DecodedImage img;
    const auto fail = [&](const char* m) {
        img.error = m;
        return img;
    };
    if (d.size() < 4 || d[0] != 0xFF || d[1] != 0xD8) return fail("no SOI");
    if (d[d.size() - 2] != 0xFF || d[d.size() - 1] != 0xD9) return fail("no EOI");
    int q[4][64] = {};
    Huff dc[4], ac[4];
    struct Comp {
        int id = 0, h = 1, v = 1, tq = 0;
        int dcTable = 0, acTable = 0;
    } comp[3];
    int ncomp = 0, w = 0, h = 0;
    std::size_t pos = 2;
    bool sawSof = false;
    while (pos + 4 <= d.size()) {
        if (d[pos] != 0xFF) return fail("expected marker");
        const int m = d[pos + 1];
        const std::size_t len = (std::size_t(d[pos + 2]) << 8) | d[pos + 3];
        const std::size_t body = pos + 4, end = pos + 2 + len;
        if (end > d.size()) return fail("segment past EOF");
        if (m == 0xE0) {
        } else if (m == 0xDB) {
            std::size_t p = body;
            while (p < end) {
                const int pq = d[p] >> 4, tq = d[p] & 15;
                ++p;
                if (pq != 0 || tq > 3) return fail("unsupported DQT");
                for (int i = 0; i < 64; ++i) q[tq][zigzag()[i]] = d[p++];
            }
        } else if (m == 0xC0) {
            if (d[body] != 8) return fail("precision");
            h = (d[body + 1] << 8) | d[body + 2];
            w = (d[body + 3] << 8) | d[body + 4];
            ncomp = d[body + 5];
            if (ncomp != 3 && ncomp != 1) return fail("components");
            for (int i = 0; i < ncomp; ++i) {
                comp[i].id = d[body + 6 + i * 3];
                comp[i].h = d[body + 7 + i * 3] >> 4;
                comp[i].v = d[body + 7 + i * 3] & 15;
                comp[i].tq = d[body + 8 + i * 3];
                img.compSampling[i][0] = comp[i].h;
                img.compSampling[i][1] = comp[i].v;
            }
            sawSof = true;
        } else if (m == 0xC4) {
            std::size_t p = body;
            while (p < end) {
                const int cls = d[p] >> 4, id = d[p] & 15;
                ++p;
                std::uint8_t counts[16];
                int total = 0;
                for (int i = 0; i < 16; ++i) total += (counts[i] = d[p + static_cast<std::size_t>(i)]);
                p += 16;
                std::vector<std::uint8_t> v(d.begin() + static_cast<std::ptrdiff_t>(p), d.begin() + static_cast<std::ptrdiff_t>(p) + total);
                p += static_cast<std::size_t>(total);
                (cls == 0 ? dc : ac)[id & 3].build(counts, std::move(v));
            }
        } else if (m == 0xDA) {
            if (!sawSof) return fail("SOS before SOF");
            const int ns = d[body];
            if (ns != ncomp) return fail("non-interleaved scan");
            for (int i = 0; i < ns; ++i) {
                comp[i].dcTable = d[body + 2 + i * 2] >> 4;
                comp[i].acTable = d[body + 2 + i * 2] & 15;
            }
            int hmax = 1, vmax = 1;
            for (int i = 0; i < ncomp; ++i) {
                hmax = std::max(hmax, comp[i].h);
                vmax = std::max(vmax, comp[i].v);
            }
            const int mcuW = 8 * hmax, mcuH = 8 * vmax;
            const int mcusX = (w + mcuW - 1) / mcuW, mcusY = (h + mcuH - 1) / mcuH;
            std::vector<std::vector<float>> planes(static_cast<std::size_t>(ncomp));
            std::vector<int> pw(static_cast<std::size_t>(ncomp)), ph(static_cast<std::size_t>(ncomp));
            for (int c = 0; c < ncomp; ++c) {
                pw[static_cast<std::size_t>(c)] = mcusX * comp[c].h * 8;
                ph[static_cast<std::size_t>(c)] = mcusY * comp[c].v * 8;
                planes[static_cast<std::size_t>(c)].assign(static_cast<std::size_t>(pw[static_cast<std::size_t>(c)]) * static_cast<std::size_t>(ph[static_cast<std::size_t>(c)]), 0.0f);
            }
            // entropy data runs until the EOI
            BitIn in{&d[end], d.size() - 2 - end, 0};
            int pred[3] = {0, 0, 0};
            for (int my = 0; my < mcusY; ++my)
                for (int mx = 0; mx < mcusX; ++mx)
                    for (int c = 0; c < ncomp; ++c)
                        for (int by = 0; by < comp[c].v; ++by)
                            for (int bx = 0; bx < comp[c].h; ++bx) {
                                int zz[64] = {};
                                const Huff& hd = dc[comp[c].dcTable];
                                const Huff& ha = ac[comp[c].acTable];
                                if (!hd.defined || !ha.defined) return fail("missing Huffman table");
                                const int t = in.decode(hd);
                                if (t > 11) return fail("bad DC category");
                                pred[c] += BitIn::extend(in.receive(t), t);
                                zz[0] = pred[c];
                                for (int k = 1; k < 64;) {
                                    const int rs = in.decode(ha);
                                    if (in.bad) return fail("entropy data error");
                                    const int r = rs >> 4, s = rs & 15;
                                    if (s == 0) {
                                        if (r == 15) {
                                            k += 16;
                                            continue;
                                        }
                                        break;  // EOB
                                    }
                                    k += r;
                                    if (k > 63) return fail("coefficient overflow");
                                    zz[k++] = BitIn::extend(in.receive(s), s);
                                }
                                if (in.bad) return fail("entropy data error");
                                float coef[64], px[64];
                                for (int i = 0; i < 64; ++i) coef[zigzag()[i]] = static_cast<float>(zz[i] * q[comp[c].tq][zigzag()[i]]);
                                idct(coef, px);
                                const int ox = (mx * comp[c].h + bx) * 8, oy = (my * comp[c].v + by) * 8;
                                auto& pl = planes[static_cast<std::size_t>(c)];
                                for (int y = 0; y < 8; ++y)
                                    for (int x = 0; x < 8; ++x)
                                        pl[static_cast<std::size_t>(oy + y) * static_cast<std::size_t>(pw[static_cast<std::size_t>(c)]) + static_cast<std::size_t>(ox + x)] = px[y * 8 + x] + 128.0f;
                            }
            if (in.pos != in.n && in.cnt == 0 && in.pos + 1 < in.n) return fail("garbage after scan");
            img.width = w;
            img.height = h;
            img.rgb.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 3);
            const auto clamp8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(static_cast<int>(std::lround(v)), 0, 255)); };
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) {
                    float comps[3] = {0, 128, 128};
                    for (int c = 0; c < ncomp; ++c) {
                        const int sx = x * comp[c].h / hmax, sy = y * comp[c].v / vmax;  // replicate
                        comps[c] = planes[static_cast<std::size_t>(c)][static_cast<std::size_t>(sy) * static_cast<std::size_t>(pw[static_cast<std::size_t>(c)]) + static_cast<std::size_t>(sx)];
                    }
                    const float Y = comps[0], cb = comps[1] - 128.0f, cr = comps[2] - 128.0f;
                    std::uint8_t* o = &img.rgb[(static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)) * 3];
                    o[0] = clamp8(Y + 1.402f * cr);
                    o[1] = clamp8(Y - 0.344136f * cb - 0.714136f * cr);
                    o[2] = clamp8(Y + 1.772f * cb);
                }
            img.ok = true;
            return img;
        }
        pos = end;
    }
    return fail("no SOS");
}

// ------------------------------------------------------------------ synthetic signals
// BGRA frame with colour bars, a luminance ramp, a moving box and fine detail (a hard test for JPEG).
inline std::shared_ptr<video::Frame> makeFrame(int w, int h, std::uint64_t index) {
    auto f = std::make_shared<video::Frame>();
    f->width = w;
    f->height = h;
    f->stride = w * 4;
    f->index = index;
    f->data.resize(static_cast<std::size_t>(f->stride) * static_cast<std::size_t>(h));
    static const std::uint8_t bars[7][3] = {{191, 191, 191}, {191, 191, 0}, {0, 191, 191}, {0, 191, 0}, {191, 0, 191}, {191, 0, 0}, {0, 0, 191}};
    const int boxX = static_cast<int>((index * 17) % static_cast<std::uint64_t>(w - 64));
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            std::uint8_t r, g, b;
            if (y < h * 2 / 3) {
                const auto& c = bars[x * 7 / w];
                r = c[0], g = c[1], b = c[2];
            } else {
                const int v = x * 255 / (w - 1);
                r = g = b = static_cast<std::uint8_t>(v);
                if (y > h * 5 / 6) {  // fine stripes
                    const int s = ((x / 3) & 1) ? 235 : 20;
                    r = g = b = static_cast<std::uint8_t>(s);
                }
            }
            if (x >= boxX && x < boxX + 64 && y >= 40 && y < 104) r = 255, g = 255, b = 255;
            std::uint8_t* p = &f->data[static_cast<std::size_t>(y) * static_cast<std::size_t>(f->stride) + static_cast<std::size_t>(x) * 4];
            p[0] = b, p[1] = g, p[2] = r, p[3] = 255;
        }
    return f;
}

// Interleaved stereo: 440 Hz left, 660 Hz right, plus a little deterministic noise.
inline std::vector<float> makeAudio(std::size_t startFrame, std::size_t frames, int rate, int channels = 2, float noise = 0.002f) {
    std::vector<float> v(frames * static_cast<std::size_t>(channels));
    std::uint32_t seed = static_cast<std::uint32_t>(startFrame) * 2654435761u + 12345u;
    for (std::size_t i = 0; i < frames; ++i) {
        const double t = static_cast<double>(startFrame + i) / rate;
        for (int c = 0; c < channels; ++c) {
            seed = seed * 1664525u + 1013904223u;
            const float n = (static_cast<float>(seed >> 8) / 8388608.0f - 1.0f) * noise;
            const double fr = 440.0 * (c + 1) * (c == 0 ? 1.0 : 0.75);
            v[i * static_cast<std::size_t>(channels) + static_cast<std::size_t>(c)] = static_cast<float>(0.5 * std::sin(2 * 3.14159265358979323846 * fr * t) + n);
        }
    }
    return v;
}

inline double psnrRgbVsBgra(const std::vector<std::uint8_t>& rgb, const video::Frame& ref) {
    double se = 0;
    const int w = ref.width, h = ref.height;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const std::uint8_t* p = &ref.data[static_cast<std::size_t>(y) * static_cast<std::size_t>(ref.stride) + static_cast<std::size_t>(x) * 4];
            const std::uint8_t* q = &rgb[(static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)) * 3];
            const double dr = double(p[2]) - q[0], dg = double(p[1]) - q[1], db = double(p[0]) - q[2];
            se += dr * dr + dg * dg + db * db;
        }
    se /= static_cast<double>(w) * h * 3;
    return se == 0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / se);
}

}  // namespace hb::rectest
