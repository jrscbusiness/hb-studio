#include <algorithm>

#include "rtsp/depacketizer.hpp"

namespace hb::rtsp {
namespace detail {

namespace {
constexpr std::size_t kMaxStoredParams = 4;

class H264Depacketizer final : public Depacketizer {
public:
    explicit H264Depacketizer(const DepacketizerConfig& cfg) : Depacketizer(cfg) { loadSdpParams(); }

private:
    void loadSdpParams() {
        for (const auto& ps : cfg_.paramSets) {
            if (ps.empty()) continue;
            storeParam(ps.data(), ps.size());
        }
    }

    void storeParam(const std::uint8_t* nal, std::size_t n) {
        int type = nal[0] & 0x1f;
        auto& list = type == 7 ? sps_ : pps_;
        Bytes b(nal, nal + n);
        if (std::find(list.begin(), list.end(), b) != list.end()) return;
        list.push_back(std::move(b));
        if (list.size() > kMaxStoredParams) list.erase(list.begin());
    }

    bool isKeyNal(const std::uint8_t* nal, std::size_t) const override { return (nal[0] & 0x1f) == 5; }

    void noteNal(const std::uint8_t* nal, std::size_t n) override {
        int type = nal[0] & 0x1f;
        if (type == 7) {
            hasSps_ = true;
            storeParam(nal, n);
        } else if (type == 8) {
            hasPps_ = true;
            storeParam(nal, n);
        } else if (type == 9 && nalCount_ == 0) {
            auPrefixEnd_ = cur_.data.size();
        }
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
        if (p.empty() || (p[0] & 0x80)) {
            ++stats_.truncatedPayloads;
            return;
        }
        int type = p[0] & 0x1f;
        if (type >= 1 && type <= 23) {
            onLoss();  // a complete NAL while a fragment is open means the fragment's tail is lost
            addNal(p.data(), p.size());
        } else if (type == 24) {
            onLoss();
            parseStapA(p);
        } else if (type == 28) {
            parseFuA(p);
        } else {
            ++stats_.unsupportedNals;  // 25-27 STAP-B/MTAP, 29 FU-B, 0/30/31 reserved
        }
    }

    void parseStapA(ByteView p) {
        std::size_t pos = 1;
        bool any = false;
        while (pos < p.size()) {
            if (pos + 2 > p.size()) break;
            std::size_t sz = (std::size_t(p[pos]) << 8) | p[pos + 1];
            pos += 2;
            if (sz == 0 || pos + sz > p.size()) {
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
        if (pos != p.size() || !any) {
            if (!any || pos > p.size()) ++stats_.truncatedPayloads;
        }
    }

    void parseFuA(ByteView p) {
        if (p.size() < 3) {
            ++stats_.truncatedPayloads;
            return;
        }
        std::uint8_t ind = p[0], fh = p[1];
        bool start = fh & 0x80, end = fh & 0x40;
        int type = fh & 0x1f;
        if ((start && end) || (fh & 0x20) || type == 0) {
            ++stats_.truncatedPayloads;
            return;
        }
        if (start) {
            onLoss();  // previous fragment never ended
            fuStart_ = beginNal();
            cur_.data.push_back(static_cast<std::uint8_t>((ind & 0xe0) | type));
            appendNalBytes(p.data() + 2, p.size() - 2);
            fuActive_ = true;
            return;
        }
        if (!fuActive_) {  // start fragment was lost or we joined mid-NAL
            ++stats_.discardedFragments;
            markDamaged();
            return;
        }
        appendNalBytes(p.data() + 2, p.size() - 2);
        if (end) {
            fuActive_ = false;
            completeNal(fuStart_);
        }
    }

    std::vector<Bytes> sps_, pps_;
    bool fuActive_ = false;
    std::size_t fuStart_ = 0;
};
}  // namespace

std::unique_ptr<Depacketizer> makeH264Depacketizer(const DepacketizerConfig& cfg) {
    return std::make_unique<H264Depacketizer>(cfg);
}

}  // namespace detail
}  // namespace hb::rtsp
