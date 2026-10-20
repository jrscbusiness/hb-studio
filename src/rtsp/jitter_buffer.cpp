#include "rtsp/jitter_buffer.hpp"

#include <algorithm>
#include <cmath>

namespace hb::rtsp {

JitterConfig JitterConfig::forMode(core::LatencyMode mode, bool inOrderTransport, std::uint32_t clockRate) {
    auto lp = core::latencyParams(mode);
    JitterConfig c;
    int wait = lp.networkBufferMs;
    if (inOrderTransport) wait = std::min(wait, 20);
    c.maxReorderWait = Millis(wait);
    c.maxPackets = mode == core::LatencyMode::Low ? 128 : mode == core::LatencyMode::Balanced ? 512 : 2048;
    c.dropOldestOnOverflow = lp.dropOldestOnOverflow;
    c.clockRate = clockRate ? clockRate : 90000;
    return c;
}

void BitrateMeter::add(TimePoint t, std::size_t bytes) {
    samples_.emplace_back(t, bytes);
    sum_ += bytes;
    trim(t);
}
void BitrateMeter::trim(TimePoint now) {
    while (!samples_.empty() && now - samples_.front().first > window_) {
        sum_ -= samples_.front().second;
        samples_.pop_front();
    }
}
double BitrateMeter::bitsPerSecond(TimePoint now) {
    trim(now);
    double secs = std::chrono::duration<double>(window_).count();
    return secs > 0 ? double(sum_) * 8.0 / secs : 0.0;
}

JitterBuffer::JitterBuffer(JitterConfig cfg) : cfg_(cfg) {
    if (cfg_.clockRate == 0) cfg_.clockRate = 90000;
    if (cfg_.maxPackets == 0) cfg_.maxPackets = 1;
}

void JitterBuffer::reset() {
    stats_ = {};
    started_ = false;
    store_.clear();
    gapSince_.reset();
    haveBadSeq_ = false;
    haveTransit_ = false;
    jitter_ = 0;
    releasedMask_ = 0;
    prevExpected_ = prevReceived_ = 0;
    skipped_ = 0;
    candidateCount_ = 0;
}

void JitterBuffer::updateJitter(const RtpPacketView& pkt, TimePoint now) {
    double arrival = std::chrono::duration<double>(now.time_since_epoch()).count() * cfg_.clockRate;
    if (haveTransit_) {
        double d = (arrival - lastArrival_) - double(static_cast<std::int32_t>(pkt.timestamp - lastTs_));
        jitter_ += (std::fabs(d) - jitter_) / 16.0;
    }
    haveTransit_ = true;
    lastArrival_ = arrival;
    lastTs_ = pkt.timestamp;
}

void JitterBuffer::push(const RtpPacketView& pkt, TimePoint now, std::vector<OwnedRtpPacket>& out) {
    if (cfg_.expectedPayloadType >= 0 && pkt.payloadType != cfg_.expectedPayloadType) {
        ++stats_.foreign;
        return;
    }
    if (started_ && pkt.ssrc != ssrc_) {
        ++stats_.foreign;
        if (pkt.ssrc == candidateSsrc_) ++candidateCount_;
        else { candidateSsrc_ = pkt.ssrc; candidateCount_ = 1; }
        if (candidateCount_ < 3) return;
        // Sender restarted with a new SSRC: continue the extended sequence space.
        ++stats_.resyncs;
        store_.clear();
        gapSince_.reset();
        started_ = false;
        restartExt_ = maxExt_;
        haveRestart_ = true;
    }
    candidateCount_ = 0;

    std::int64_t ext;
    if (!started_) {
        started_ = true;
        ssrc_ = pkt.ssrc;
        maxSeq_ = pkt.sequence;
        cycles_ = haveRestart_ ? (((restartExt_ >> 16) + 1) << 16) : 0;
        ext = cycles_ + pkt.sequence;
        if (haveRestart_) {
            skipped_ += ext - restartExt_ - 1;
        } else {
            baseExt_ = ext;
        }
        haveRestart_ = false;
        nextExt_ = ext;
        maxExt_ = ext;
        releasedMask_ = 0;
        haveBadSeq_ = false;
    } else {
        std::uint16_t delta = static_cast<std::uint16_t>(pkt.sequence - maxSeq_);
        if (delta < cfg_.maxDropout) {
            if (pkt.sequence < maxSeq_) cycles_ += 65536;
            maxSeq_ = pkt.sequence;
            ext = cycles_ + pkt.sequence;
            haveBadSeq_ = false;
        } else if (delta <= 65536 - cfg_.maxMisorder) {
            // Big jump: accept only if the next packet continues from it (RFC 3550 A.1).
            if (haveBadSeq_ && pkt.sequence == badSeq_) {
                ++stats_.resyncs;
                store_.clear();
                gapSince_.reset();
                cycles_ = ((maxExt_ >> 16) + 1) << 16;
                maxSeq_ = pkt.sequence;
                ext = cycles_ + pkt.sequence;
                skipped_ += ext - maxExt_ - 1;
                nextExt_ = ext;
                maxExt_ = ext - 1;  // so the max update below applies
                releasedMask_ = 0;
                haveBadSeq_ = false;
            } else {
                badSeq_ = static_cast<std::uint16_t>(pkt.sequence + 1);
                haveBadSeq_ = true;
                ++stats_.foreign;
                return;
            }
        } else {
            ext = cycles_ + pkt.sequence;
            if (pkt.sequence > maxSeq_) ext -= 65536;  // belongs to the previous cycle
        }
    }

    stats_.bytes += pkt.payload.size() + 12;

    if (ext < nextExt_) {  // already released (or given up on)
        std::int64_t dist = nextExt_ - 1 - ext;
        if (dist < 64 && ((releasedMask_ >> dist) & 1)) {
            ++stats_.duplicates;
        } else {
            ++stats_.late;
            ++stats_.received;
            updateJitter(pkt, now);
        }
        return;
    }
    if (store_.count(ext)) {
        ++stats_.duplicates;
        return;
    }
    // A full store rejects newcomers, except the very packet that closes the gap (it drains the store).
    if (store_.size() >= cfg_.maxPackets && !cfg_.dropOldestOnOverflow && ext != nextExt_) {
        ++stats_.overflowDrops;
        return;
    }
    ++stats_.received;
    if (ext < maxExt_) ++stats_.reordered;
    if (ext > maxExt_) maxExt_ = ext;
    updateJitter(pkt, now);

    OwnedRtpPacket o;
    o.payloadType = pkt.payloadType;
    o.marker = pkt.marker;
    o.sequence = pkt.sequence;
    o.timestamp = pkt.timestamp;
    o.ssrc = pkt.ssrc;
    o.payload.assign(pkt.payload.begin(), pkt.payload.end());
    o.arrival = now;
    store_.emplace(ext, std::move(o));
    drain(now, out);
}

void JitterBuffer::poll(TimePoint now, std::vector<OwnedRtpPacket>& out) { drain(now, out); }

void JitterBuffer::drain(TimePoint now, std::vector<OwnedRtpPacket>& out) {
    while (!store_.empty()) {
        auto it = store_.begin();
        if (it->first == nextExt_) {
            out.push_back(std::move(it->second));
            store_.erase(it);
            releasedMask_ = (releasedMask_ << 1) | 1;
            ++nextExt_;
            gapSince_.reset();
            continue;
        }
        if (!gapSince_) gapSince_ = now;
        bool timeout = now - *gapSince_ >= cfg_.maxReorderWait;
        bool overflow = cfg_.dropOldestOnOverflow && store_.size() >= cfg_.maxPackets;
        if (!timeout && !overflow) break;
        std::int64_t skip = it->first - nextExt_;
        releasedMask_ = skip >= 64 ? 0 : (releasedMask_ << skip);
        nextExt_ = it->first;
        gapSince_.reset();
    }
    if (store_.empty()) gapSince_.reset();
}

const RtpStats& JitterBuffer::stats() {
    stats_.jitter = static_cast<std::uint32_t>(jitter_);
    stats_.extHighestSeq = static_cast<std::uint32_t>(maxExt_);
    if (started_ || stats_.received) {
        std::int64_t exp = maxExt_ - baseExt_ + 1 - skipped_;
        stats_.expected = exp > 0 ? std::uint64_t(exp) : 0;
        stats_.lost = stats_.expected > stats_.received ? stats_.expected - stats_.received : 0;
        stats_.lossPercent = stats_.expected ? 100.0 * double(stats_.lost) / double(stats_.expected) : 0.0;
    }
    return stats_;
}

ReportBlock JitterBuffer::reportBlock(std::uint32_t lastSr, std::uint32_t delaySinceSr) {
    const RtpStats& s = stats();
    ReportBlock b;
    b.sourceSsrc = ssrc_;
    std::uint64_t expInt = s.expected - prevExpected_;
    std::uint64_t recInt = s.received - prevReceived_;
    prevExpected_ = s.expected;
    prevReceived_ = s.received;
    if (expInt > recInt && expInt > 0) {
        std::uint64_t lostInt = expInt - recInt;
        b.fractionLost = static_cast<std::uint8_t>(std::min<std::uint64_t>(255, (lostInt << 8) / expInt));
    }
    b.cumulativeLost = static_cast<std::uint32_t>(std::min<std::uint64_t>(s.lost, 0xffffff));
    b.extHighestSeq = s.extHighestSeq;
    b.jitter = s.jitter;
    b.lastSr = lastSr;
    b.delaySinceLastSr = delaySinceSr;
    return b;
}

}  // namespace hb::rtsp
