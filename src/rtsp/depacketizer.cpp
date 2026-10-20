#include "rtsp/depacketizer.hpp"

namespace hb::rtsp {

namespace detail {
std::unique_ptr<Depacketizer> makeH264Depacketizer(const DepacketizerConfig& cfg);
std::unique_ptr<Depacketizer> makeH265Depacketizer(const DepacketizerConfig& cfg);
}  // namespace detail

namespace {
constexpr std::uint8_t kStartCode[4] = {0, 0, 0, 1};
}

std::unique_ptr<Depacketizer> Depacketizer::create(const DepacketizerConfig& cfg) {
    return cfg.codec == Codec::H264 ? detail::makeH264Depacketizer(cfg) : detail::makeH265Depacketizer(cfg);
}

Depacketizer::Depacketizer(const DepacketizerConfig& cfg) : cfg_(cfg) {
    if (cfg_.clockRate == 0) cfg_.clockRate = 90000;
    cur_.codec = cfg_.codec;
}

void Depacketizer::reset() {
    stats_ = {};
    clearCurrent();
    auOpen_ = false;
    haveSeq_ = false;
    pendingLoss_ = false;
    waitingKeyframe_ = true;
    haveTs_ = false;
    ptsTicks_ = 0;
    resetCodecState();
}

void Depacketizer::clearCurrent() {
    cur_.data.clear();
    cur_.damaged = false;
    cur_.keyframe = false;
    curKey_ = false;
    hasSps_ = hasPps_ = hasVps_ = false;
    auPrefixEnd_ = 0;
    nalCount_ = 0;
    discardAu_ = false;
}

std::int64_t Depacketizer::ptsFor(std::uint32_t ts) {
    if (!haveTs_) {
        haveTs_ = true;
        ptsTicks_ = 0;
    } else {
        ptsTicks_ += static_cast<std::int32_t>(ts - lastTs_);
    }
    lastTs_ = ts;
    std::int64_t clk = cfg_.clockRate;
    std::int64_t sec = ptsTicks_ / clk, rem = ptsTicks_ % clk;
    return sec * 1000000 + rem * 1000000 / clk;
}

std::size_t Depacketizer::beginNal() {
    std::size_t off = cur_.data.size();
    cur_.data.insert(cur_.data.end(), kStartCode, kStartCode + 4);
    return off;
}

void Depacketizer::appendNalBytes(const std::uint8_t* d, std::size_t n) {
    cur_.data.insert(cur_.data.end(), d, d + n);
}

void Depacketizer::abortNalAt(std::size_t off) {
    if (off <= cur_.data.size()) cur_.data.resize(off);
}

void Depacketizer::completeNal(std::size_t startOffset) {
    const std::uint8_t* nal = cur_.data.data() + startOffset + 4;
    std::size_t n = cur_.data.size() - startOffset - 4;
    if (n == 0) {
        abortNalAt(startOffset);
        return;
    }
    if (isKeyNal(nal, n)) curKey_ = true;
    noteNal(nal, n);
    ++nalCount_;
}

void Depacketizer::addNal(const std::uint8_t* nal, std::size_t n) {
    if (n == 0) return;
    std::size_t s = beginNal();
    appendNalBytes(nal, n);
    completeNal(s);
}

void Depacketizer::push(std::uint16_t seq, std::uint32_t ts, bool marker, ByteView payload, TimePoint arrival) {
    ++stats_.packets;
    if (haveSeq_) {
        std::uint16_t d = static_cast<std::uint16_t>(seq - expectedSeq_);
        if (d != 0) {
            if (d >= 0x8000) {  // older than what we already consumed: duplicate / late
                ++stats_.oldOrDuplicate;
                return;
            }
            ++stats_.seqGaps;
            pendingLoss_ = true;
            onLoss();
        }
    }
    haveSeq_ = true;
    expectedSeq_ = static_cast<std::uint16_t>(seq + 1);

    if (auOpen_ && ts != curTs_) {
        if (pendingLoss_) cur_.damaged = true;
        emitCurrent(arrival);
    }
    if (!auOpen_) {
        auOpen_ = true;
        curTs_ = ts;
        cur_.ptsUs = ptsFor(ts);
        cur_.rtpTimestamp = ts;
        cur_.damaged = pendingLoss_;
    } else if (pendingLoss_) {
        cur_.damaged = true;
    }
    pendingLoss_ = false;

    if (!discardAu_) {
        parsePayload(payload);
        if (auOverflow()) {
            ++stats_.oversizeAus;
            onLoss();
            cur_.data.clear();
            nalCount_ = 0;
            discardAu_ = true;
        }
    }
    if (marker) emitCurrent(arrival);
}

void Depacketizer::flush(TimePoint now) { emitCurrent(now); }

void Depacketizer::emitCurrent(TimePoint t) {
    if (!auOpen_) return;
    auOpen_ = false;
    onLoss();  // abandon any unfinished fragment (it would be a corrupt NAL)
    if (discardAu_) {
        ++stats_.droppedAus;
        clearCurrent();
        return;
    }
    if (nalCount_ == 0 && !cur_.damaged) {  // e.g. only unsupported NAL types
        clearCurrent();
        return;
    }
    if (cur_.damaged) {
        // Counted even when nothing usable is left (all fragments lost): the frame is gone and
        // everything that references it is undecodable.
        ++stats_.damagedAus;
        if (cfg_.dropUntilKeyframeAfterLoss) {
            waitingKeyframe_ = true;
            ++stats_.droppedAus;
            clearCurrent();
            return;
        }
        if (nalCount_ == 0) {
            ++stats_.droppedAus;
            clearCurrent();
            return;
        }
    }
    if (waitingKeyframe_) {
        if (!curKey_) {
            ++stats_.droppedAus;
            clearCurrent();
            return;
        }
        waitingKeyframe_ = false;
    }
    if (curKey_) prependParams(cur_.data, auPrefixEnd_);
    cur_.codec = cfg_.codec;
    cur_.keyframe = curKey_;
    cur_.completedAt = t;
    ++stats_.accessUnits;
    if (curKey_) ++stats_.keyframes;
    AccessUnit out = std::move(cur_);
    cur_ = AccessUnit{};
    cur_.codec = cfg_.codec;
    clearCurrent();
    if (sink_) sink_(std::move(out));
}

}  // namespace hb::rtsp
