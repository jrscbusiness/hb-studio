#include "recording/flac_writer.hpp"

#include <algorithm>
#include <cstring>

#include "recording/pcm.hpp"

namespace hb::rec {

namespace {

struct CrcTables {
    std::uint8_t t8[256];
    std::uint16_t t16[256];
    CrcTables() {
        for (int i = 0; i < 256; ++i) {
            std::uint8_t c8 = static_cast<std::uint8_t>(i);
            std::uint16_t c16 = static_cast<std::uint16_t>(i << 8);
            for (int k = 0; k < 8; ++k) {
                c8 = static_cast<std::uint8_t>((c8 & 0x80) ? ((c8 << 1) ^ 0x07) : (c8 << 1));
                c16 = static_cast<std::uint16_t>((c16 & 0x8000) ? ((c16 << 1) ^ 0x8005) : (c16 << 1));
            }
            t8[i] = c8;
            t16[i] = c16;
        }
    }
};
const CrcTables& crcTables() {
    static const CrcTables t;
    return t;
}
std::uint8_t crc8(const std::uint8_t* p, std::size_t n) {
    const auto& t = crcTables();
    std::uint8_t c = 0;
    for (std::size_t i = 0; i < n; ++i) c = t.t8[c ^ p[i]];
    return c;
}
std::uint16_t crc16(const std::uint8_t* p, std::size_t n) {
    const auto& t = crcTables();
    std::uint16_t c = 0;
    for (std::size_t i = 0; i < n; ++i) c = static_cast<std::uint16_t>((c << 8) ^ t.t16[(c >> 8) ^ p[i]]);
    return c;
}

// MSB-first bit writer appending whole bytes to a vector.
class BitWriter {
public:
    explicit BitWriter(std::vector<std::uint8_t>& out) : out_(out) {}
    void put(std::uint32_t value, int bits) {  // bits in [0, 32]
        if (bits == 0) return;
        const std::uint64_t mask = (bits == 32) ? 0xFFFFFFFFull : ((1ull << bits) - 1);
        acc_ = (acc_ << bits) | (value & mask);
        n_ += bits;
        while (n_ >= 8) {
            out_.push_back(static_cast<std::uint8_t>(acc_ >> (n_ - 8)));
            n_ -= 8;
        }
        acc_ &= (1ull << n_) - 1;
    }
    void putSigned(std::int32_t v, int bits) { put(static_cast<std::uint32_t>(v), bits); }
    void unary(std::uint32_t q) {  // q zeros then a one
        while (q >= 31) {
            put(0, 31);
            q -= 31;
        }
        put(1, static_cast<int>(q) + 1);
    }
    void align() {
        if (n_ > 0) put(0, 8 - n_);
    }

private:
    std::vector<std::uint8_t>& out_;
    std::uint64_t acc_ = 0;
    int n_ = 0;
};

inline std::uint32_t zigzag(std::int32_t r) {
    return (static_cast<std::uint32_t>(r) << 1) ^ static_cast<std::uint32_t>(r >> 31);
}

constexpr int kMaxPo = 8;
constexpr int kMaxK = 30;

struct Plan {
    enum Type { Constant, Verbatim, Fixed } type = Verbatim;
    int order = 0, po = 0;
    bool method1 = false;
    std::uint64_t bits = 0;
    int params[1 << kMaxPo] = {};
};

// Chooses the cheapest representation of one channel signal of `n` samples at `bps` bits.
struct Planner {
    std::vector<std::int32_t> res[5];

    void plan(const std::int32_t* s, int n, int bps, Plan& best) {
        best.type = Plan::Verbatim;
        best.bits = 8 + static_cast<std::uint64_t>(n) * static_cast<std::uint64_t>(bps);
        bool constant = true;
        for (int i = 1; i < n; ++i)
            if (s[i] != s[0]) {
                constant = false;
                break;
            }
        if (constant) {
            best.type = Plan::Constant;
            best.bits = 8 + static_cast<std::uint64_t>(bps);
            return;
        }
        for (int p = 0; p <= 4 && p < n; ++p) {
            computeResiduals(s, n, p);
            evalOrder(p, n, bps, best);
        }
    }

    // Fills res[0..p]; res[q][i] (i >= q) is the order-q fixed-predictor residual.
    void computeResiduals(const std::int32_t* s, int n, int p) {
        for (int q = 0; q <= p; ++q) {
            auto& r = res[q];
            if (r.size() < static_cast<std::size_t>(n)) r.resize(static_cast<std::size_t>(n));
            if (q == 0) {
                std::copy(s, s + n, r.begin());
            } else {
                const auto& prev = res[q - 1];
                for (int i = q; i < n; ++i) r[static_cast<std::size_t>(i)] = prev[static_cast<std::size_t>(i)] - prev[static_cast<std::size_t>(i - 1)];
            }
        }
    }

    void evalOrder(int p, int n, int bps, Plan& best) {
        int maxPo = 0;
        while (maxPo < kMaxPo && n % (1 << (maxPo + 1)) == 0 && (n >> (maxPo + 1)) > p) ++maxPo;
        if ((n >> maxPo) <= p) return;  // block too short for this order
        const int parts = 1 << maxPo, ps = n >> maxPo;
        std::uint64_t sums[kMaxPo + 1][1 << kMaxPo];
        std::uint32_t cnt[kMaxPo + 1][1 << kMaxPo];
        const std::int32_t* r = res[p].data();
        for (int j = 0; j < parts; ++j) {
            const int begin = std::max(j * ps, p), end = (j + 1) * ps;
            std::uint64_t sum = 0;
            for (int i = begin; i < end; ++i) sum += zigzag(r[i]);
            sums[maxPo][j] = sum;
            cnt[maxPo][j] = static_cast<std::uint32_t>(end - begin);
        }
        for (int po = maxPo - 1; po >= 0; --po)
            for (int j = 0; j < (1 << po); ++j) {
                sums[po][j] = sums[po + 1][2 * j] + sums[po + 1][2 * j + 1];
                cnt[po][j] = cnt[po + 1][2 * j] + cnt[po + 1][2 * j + 1];
            }
        for (int po = 0; po <= maxPo; ++po) {
            std::uint64_t total = 0;
            int ks[1 << kMaxPo];
            int maxk = 0;
            for (int j = 0; j < (1 << po); ++j) {
                std::uint64_t bestCost = ~0ull;
                int bestK = 0;
                for (int k = 0; k <= kMaxK; ++k) {
                    const std::uint64_t c = static_cast<std::uint64_t>(cnt[po][j]) * static_cast<std::uint64_t>(k + 1) + (sums[po][j] >> k);
                    if (c < bestCost) {
                        bestCost = c;
                        bestK = k;
                    }
                }
                ks[j] = bestK;
                maxk = std::max(maxk, bestK);
                total += bestCost;
            }
            const bool m1 = maxk > 14;
            const std::uint64_t bits = 8 + static_cast<std::uint64_t>(p) * static_cast<std::uint64_t>(bps) + 2 + 4 +
                                       static_cast<std::uint64_t>(1 << po) * (m1 ? 5 : 4) + total;
            if (bits < best.bits) {
                best.type = Plan::Fixed;
                best.order = p;
                best.po = po;
                best.method1 = m1;
                best.bits = bits;
                std::copy(ks, ks + (1 << po), best.params);
            }
        }
    }

    void write(BitWriter& bw, const std::int32_t* s, int n, int bps, const Plan& plan) {
        if (plan.type == Plan::Constant) {
            bw.put(0, 8);  // pad(1) type(6)=0 wasted(1)=0
            bw.putSigned(s[0], bps);
            return;
        }
        if (plan.type == Plan::Verbatim) {
            bw.put(1u << 1, 8);
            for (int i = 0; i < n; ++i) bw.putSigned(s[i], bps);
            return;
        }
        const int p = plan.order;
        computeResiduals(s, n, p);
        bw.put(static_cast<std::uint32_t>((8 + p) << 1), 8);
        for (int i = 0; i < p; ++i) bw.putSigned(s[i], bps);
        bw.put(plan.method1 ? 1u : 0u, 2);
        bw.put(static_cast<std::uint32_t>(plan.po), 4);
        const int parts = 1 << plan.po, ps = n >> plan.po;
        const std::int32_t* r = res[p].data();
        for (int j = 0; j < parts; ++j) {
            const int k = plan.params[j];
            bw.put(static_cast<std::uint32_t>(k), plan.method1 ? 5 : 4);
            const int begin = std::max(j * ps, p), end = (j + 1) * ps;
            for (int i = begin; i < end; ++i) {
                const std::uint32_t u = zigzag(r[i]);
                bw.unary(u >> k);
                bw.put(u, k);  // low k bits
            }
        }
    }
};

void putUtf8Number(std::vector<std::uint8_t>& out, std::uint64_t v) {
    if (v < 0x80) {
        out.push_back(static_cast<std::uint8_t>(v));
        return;
    }
    int extra = 1;
    while (extra < 6 && v >= (1ull << (5 * extra + 6))) ++extra;
    // lead byte: `extra+1` ones, then a zero, then the high bits
    const std::uint8_t lead = static_cast<std::uint8_t>((0xFF00 >> (extra + 1)) & 0xFF);
    out.push_back(static_cast<std::uint8_t>(lead | (v >> (6 * extra))));
    for (int i = extra - 1; i >= 0; --i) out.push_back(static_cast<std::uint8_t>(0x80 | ((v >> (6 * i)) & 0x3F)));
}

thread_local Planner gPlanner;  // reused scratch; each writer is single threaded

}  // namespace

FlacWriter::~FlacWriter() {
    std::string e;
    close(e);
}

void FlacWriter::writeStreamInfo(std::vector<std::uint8_t>& out, bool final) const {
    int minB = kBlockSize, maxB = kBlockSize;
    if (final && totalSamples_ > 0) {
        if (frameNumber_ == 1) minB = maxB = firstBlockSize_;
        // otherwise every block but the last is full; the last may be shorter and is exempt from min
    }
    out.push_back(static_cast<std::uint8_t>(minB >> 8));
    out.push_back(static_cast<std::uint8_t>(minB));
    out.push_back(static_cast<std::uint8_t>(maxB >> 8));
    out.push_back(static_cast<std::uint8_t>(maxB));
    const std::uint32_t minF = final && frameNumber_ > 0 ? minFrameBytes_ : 0;
    const std::uint32_t maxF = final ? maxFrameBytes_ : 0;
    for (int i = 2; i >= 0; --i) out.push_back(static_cast<std::uint8_t>(minF >> (8 * i)));
    for (int i = 2; i >= 0; --i) out.push_back(static_cast<std::uint8_t>(maxF >> (8 * i)));
    const std::uint64_t packed = (static_cast<std::uint64_t>(sampleRate_) << 44) |
                                 (static_cast<std::uint64_t>(channels_ - 1) << 41) |
                                 (static_cast<std::uint64_t>(bps_ - 1) << 36) | (final ? totalSamples_ : 0);
    for (int i = 7; i >= 0; --i) out.push_back(static_cast<std::uint8_t>(packed >> (8 * i)));
    if (final) {
        detail::Md5 m = md5_;  // finish a copy
        const auto d = m.finish();
        out.insert(out.end(), d.begin(), d.end());
    } else {
        out.insert(out.end(), 16, 0);
    }
}

bool FlacWriter::open(const std::string& pathUtf8, const RecordingProfile& profile, const VideoFormat&,
                      const AudioFormat& audio, std::string& error) {
    if (!closed_) {
        error = "writer ja aberto";
        return false;
    }
    if (!audio.present() || audio.channels > 8 || audio.sampleRate > 655350) {
        error = "formato de audio nao suportado pelo FLAC (1 a 8 canais, ate 655 kHz)";
        return false;
    }
    bps_ = detail::normalizeBits(profile.audioBitsPerSample) == 16 ? 16 : 24;
    channels_ = audio.channels;
    sampleRate_ = audio.sampleRate;
    pending_.assign(static_cast<std::size_t>(channels_), {});
    md5_.reset();
    stats_ = {};
    totalSamples_ = frameNumber_ = 0;
    minFrameBytes_ = 0xFFFFFF;
    maxFrameBytes_ = 0;
    firstBlockSize_ = lastBlockSize_ = 0;
    failed_ = false;
    if (!file_.open(pathUtf8, error)) return false;
    std::vector<std::uint8_t> h = {'f', 'L', 'a', 'C', 0x80, 0, 0, 34};
    writeStreamInfo(h, false);
    if (!file_.write(h, error)) {
        std::string ignore;
        file_.close(ignore);
        return false;
    }
    stats_.bytesWritten = file_.size();
    closed_ = false;
    return true;
}

bool FlacWriter::writeAudio(const float* in, std::size_t frames, std::int64_t, std::string& error) {
    if (closed_ || failed_) {
        error = "writer FLAC fechado";
        return false;
    }
    std::vector<std::uint8_t> md5bytes;
    md5bytes.reserve(frames * static_cast<std::size_t>(channels_) * static_cast<std::size_t>(bps_ / 8));
    for (std::size_t i = 0; i < frames; ++i)
        for (int c = 0; c < channels_; ++c) {
            const std::int32_t q = detail::quantizePcm(in[i * static_cast<std::size_t>(channels_) + static_cast<std::size_t>(c)], bps_);
            pending_[static_cast<std::size_t>(c)].push_back(q);
            const std::uint32_t u = static_cast<std::uint32_t>(q);
            md5bytes.push_back(static_cast<std::uint8_t>(u));
            md5bytes.push_back(static_cast<std::uint8_t>(u >> 8));
            if (bps_ == 24) md5bytes.push_back(static_cast<std::uint8_t>(u >> 16));
        }
    md5_.update(md5bytes.data(), md5bytes.size());
    while (pending_[0].size() >= static_cast<std::size_t>(kBlockSize))
        if (!encodeBlock(kBlockSize, error)) {
            failed_ = true;
            return false;
        }
    stats_.audioFrames += frames;
    return true;
}

bool FlacWriter::encodeBlock(int n, std::string& error) {
    Planner& pl = gPlanner;
    const auto chan = [&](int c) { return pending_[static_cast<std::size_t>(c)].data(); };
    int assignment = channels_ - 1;  // independent channels
    const std::int32_t* sig[4] = {nullptr, nullptr, nullptr, nullptr};
    int bitsOf[4] = {bps_, bps_, bps_, bps_ + 1};
    Plan plans[4];
    int pick[2] = {0, 1};
    if (channels_ == 2) {
        const std::int32_t* L = chan(0);
        const std::int32_t* R = chan(1);
        sideBuf_.resize(static_cast<std::size_t>(n));
        midBuf_.resize(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i) {
            sideBuf_[static_cast<std::size_t>(i)] = L[i] - R[i];
            midBuf_[static_cast<std::size_t>(i)] = (L[i] + R[i]) >> 1;
        }
        sig[0] = L, sig[1] = R, sig[2] = midBuf_.data(), sig[3] = sideBuf_.data();
        for (int k = 0; k < 4; ++k) pl.plan(sig[k], n, bitsOf[k], plans[k]);
        const std::uint64_t cLR = plans[0].bits + plans[1].bits, cLS = plans[0].bits + plans[3].bits,
                            cSR = plans[3].bits + plans[1].bits, cMS = plans[2].bits + plans[3].bits;
        std::uint64_t best = cLR;
        if (cLS < best) best = cLS, assignment = 8, pick[0] = 0, pick[1] = 3;
        if (cSR < best) best = cSR, assignment = 9, pick[0] = 3, pick[1] = 1;
        if (cMS < best) best = cMS, assignment = 10, pick[0] = 2, pick[1] = 3;
    }

    frameBuf_.clear();
    frameBuf_.push_back(0xFF);
    frameBuf_.push_back(0xF8);  // sync + fixed blocksize
    const bool std4096 = n == kBlockSize;
    frameBuf_.push_back(static_cast<std::uint8_t>((std4096 ? 12 : 7) << 4));  // block size code | rate from STREAMINFO
    frameBuf_.push_back(static_cast<std::uint8_t>((assignment << 4) | ((bps_ == 16 ? 4 : 6) << 1)));
    putUtf8Number(frameBuf_, frameNumber_);
    if (!std4096) {
        frameBuf_.push_back(static_cast<std::uint8_t>((n - 1) >> 8));
        frameBuf_.push_back(static_cast<std::uint8_t>(n - 1));
    }
    frameBuf_.push_back(crc8(frameBuf_.data(), frameBuf_.size()));

    BitWriter bw(frameBuf_);
    if (channels_ == 2) {
        for (int k = 0; k < 2; ++k) pl.write(bw, sig[pick[k]], n, bitsOf[pick[k]], plans[pick[k]]);
    } else {
        for (int c = 0; c < channels_; ++c) {
            Plan pp;
            pl.plan(chan(c), n, bps_, pp);
            pl.write(bw, chan(c), n, bps_, pp);
        }
    }
    bw.align();
    const std::uint16_t c16 = crc16(frameBuf_.data(), frameBuf_.size());
    frameBuf_.push_back(static_cast<std::uint8_t>(c16 >> 8));
    frameBuf_.push_back(static_cast<std::uint8_t>(c16));

    if (!file_.write(frameBuf_, error)) return false;
    stats_.bytesWritten = file_.size();
    const auto sz = static_cast<std::uint32_t>(frameBuf_.size());
    minFrameBytes_ = std::min(minFrameBytes_, sz);
    maxFrameBytes_ = std::max(maxFrameBytes_, sz);
    if (frameNumber_ == 0) firstBlockSize_ = n;
    lastBlockSize_ = n;
    ++frameNumber_;
    totalSamples_ += static_cast<std::uint64_t>(n);
    for (auto& v : pending_) v.erase(v.begin(), v.begin() + n);
    return true;
}

bool FlacWriter::close(std::string& error) {
    if (closed_) return true;
    closed_ = true;
    std::string e;
    bool ok = true;
    if (!failed_) {
        const int rest = static_cast<int>(pending_[0].size());
        if (rest > 0) ok = encodeBlock(rest, e);
    }
    if (ok) {
        std::vector<std::uint8_t> si;
        writeStreamInfo(si, true);
        ok = file_.patch(8, si.data(), si.size(), e);
    }
    std::string ce;
    if (!file_.close(ce) && e.empty()) e = ce;
    if (!e.empty()) error = e;
    return ok && e.empty();
}

}  // namespace hb::rec
