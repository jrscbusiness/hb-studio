#include <algorithm>

#include "rtsp/depacketizer.hpp"

namespace hb::rtsp {
namespace detail {

namespace {
constexpr std::size_t kMaxStoredParams = 4;

inline int hevcType(const std::uint8_t* nal) { return (nal[0] >> 1) & 0x3f; }

// RFC 7798. Assumes sprop-max-don-diff == 0 (no DONL fields); interleaving is not supported.
class H265Depacketizer final : public Depacketizer {
public:
    explicit H265Depacketizer(const DepacketizerConfig& cfg) : Depacketizer(cfg) {
        for (const auto& ps : cfg_.paramSets)
            if (ps.size() >= 2) storeParam(ps.data(), ps.size());
    }

private:
    std::vector<Bytes>* listFor(int type) {
        return type == 32 ? &vps_ : type == 33 ? &sps_ : type == 34 ? &pps_ : nullptr;
    }
    void storeParam(const std::uint8_t* nal, std::size_t n) {
        auto* list = listFor(hevcType(nal));
        if (!list) return;
        Bytes b(nal, nal + n);
        if (std::find(list->begin(), list->end(), b) != list->end()) return;
        list->push_back(std::move(b));
        if (list->size() > kMaxStoredParams) list->erase(list->begin());
    }

    bool isKeyNal(const std::uint8_t* nal, std::size_t n) const override {
        if (n < 2) return false;
        int t = hevcType(nal);
        return t >= 16 && t <= 21;  // BLA_*, IDR_*, CRA
    }

    void noteNal(const std::uint8_t* nal, std::size_t n) override {
        if (n < 2) return;
        int t = hevcType(nal);
        if (t == 32) { hasVps_ = true; storeParam(nal, n); }
        else if (t == 33) { hasSps_ = true; storeParam(nal, n); }
        else if (t == 34) { hasPps_ = true; storeParam(nal, n); }
        else if (t == 35 && nalCount_ == 0) auPrefixEnd_ = cur_.data.size();
    }

    void prependParams(Bytes& out, std::size_t at) override {
        Bytes ins;
        auto add = [&](const std::vector<Bytes>& list) {
            for (const auto& p : list) {
                const std::uint8_t sc[4] = {0, 0, 0, 1};
                ins.insert(ins.end(), sc, sc + 4);
                ins.insert(ins.end(), p.begin(), p.end());
            }
        };
        if (!hasVps_) add(vps_);
        if (!hasSps_) add(sps_);
        if (!hasPps_) add(pps_);
        if (ins.empty()) return;
        out.insert(out.begin() + static_cast<std::ptrdiff_t>(at), ins.begin(), ins.end());
        ++stats_.paramSetInjections;
    }

    void resetCodecState() override { fuActive_ = false; }

    void onLoss() override {
        if (fuActive_) {
            abortNalAt(fuStart_);
            fuActive_ = false;
            ++stats_.discardedFragments;
            markDamaged();
        }
    }

    void parsePayload(ByteView p) override {
        if (p.size() < 3 || (p[0] & 0x80)) {  // 2-byte payload header + at least 1 byte
            ++stats_.truncatedPayloads;
            return;
        }
        int type = hevcType(p.data());
        if (type <= 47) {
            onLoss();
            addNal(p.data(), p.size());
        } else if (type == 48) {
            onLoss();
            parseAp(p);
        } else if (type == 49) {
            parseFu(p);
        } else {
            ++stats_.unsupportedNals;  // 50 PACI and reserved
        }
    }

    void parseAp(ByteView p) {
        std::size_t pos = 2;
        bool any = false;
        while (pos + 2 <= p.size()) {
            std::size_t sz = (std::size_t(p[pos]) << 8) | p[pos + 1];
            pos += 2;
            if (sz < 2 || pos + sz > p.size()) {
                ++stats_.truncatedPayloads;
                markDamaged();
                return;
            }
            if (p[pos] & 0x80) {
                ++stats_.truncatedPayloads;
                markDamaged();
            } else {
                addNal(p.data() + pos, sz);
                any = true;
            }
            pos += sz;
        }
        if (!any) ++stats_.truncatedPayloads;
    }

    void parseFu(ByteView p) {
        std::uint8_t fh = p[2];
        bool start = fh & 0x80, end = fh & 0x40;
        int type = fh & 0x3f;
        if (start && end) {
            ++stats_.truncatedPayloads;
            return;
        }
        if (start) {
            onLoss();
            fuStart_ = beginNal();
            cur_.data.push_back(static_cast<std::uint8_t>((p[0] & 0x81) | (type << 1)));
            cur_.data.push_back(p[1]);
            appendNalBytes(p.data() + 3, p.size() - 3);
            fuActive_ = true;
            return;
        }
        if (!fuActive_) {
            ++stats_.discardedFragments;
            markDamaged();
            return;
        }
        appendNalBytes(p.data() + 3, p.size() - 3);
        if (end) {
            fuActive_ = false;
            completeNal(fuStart_);
        }
    }

    std::vector<Bytes> vps_, sps_, pps_;
    bool fuActive_ = false;
    std::size_t fuStart_ = 0;
};
}  // namespace

std::unique_ptr<Depacketizer> makeH265Depacketizer(const DepacketizerConfig& cfg) {
    return std::make_unique<H265Depacketizer>(cfg);
}

}  // namespace detail
}  // namespace hb::rtsp
