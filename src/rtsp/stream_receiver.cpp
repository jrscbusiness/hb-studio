#include "rtsp/stream_receiver.hpp"

namespace hb::rtsp {

void AccessUnitQueue::push(AccessUnit&& au) {
    if (waitKeyframe_) {
        if (!au.keyframe) {
            ++dropped_;
            return;
        }
        waitKeyframe_ = false;
    }
    if (q_.size() >= capacity_) {
        if (dropOldest_) {
            // Jump to the newest keyframe we hold (or the incoming one if it is a keyframe).
            if (au.keyframe) {
                dropped_ += q_.size();
                q_.clear();
            } else {
                std::size_t lastKey = q_.size();
                for (std::size_t i = q_.size(); i-- > 0;)
                    if (q_[i].keyframe) { lastKey = i; break; }
                if (lastKey != q_.size() && lastKey > 0) {
                    dropped_ += lastKey;
                    q_.erase(q_.begin(), q_.begin() + static_cast<std::ptrdiff_t>(lastKey));
                } else {
                    dropped_ += q_.size() + 1;  // nothing decodable to keep: drop all and the newcomer
                    q_.clear();
                    waitKeyframe_ = true;
                    return;
                }
                if (q_.size() >= capacity_) {  // still full: the newcomer cannot be decoded either
                    ++dropped_;
                    waitKeyframe_ = true;
                    return;
                }
            }
        } else {
            ++dropped_;
            if (!au.keyframe) waitKeyframe_ = true;
            else { dropped_ += q_.size(); q_.clear(); }
            if (waitKeyframe_) return;
        }
    }
    q_.push_back(std::move(au));
}

std::optional<AccessUnit> AccessUnitQueue::pop() {
    if (q_.empty()) return std::nullopt;
    AccessUnit a = std::move(q_.front());
    q_.pop_front();
    return a;
}

StreamReceiver::StreamReceiver(StreamReceiverConfig cfg)
    : cfg_(std::move(cfg)), jitter_([&] {
          JitterConfig j = cfg_.jitter;
          j.clockRate = cfg_.clockRate;
          j.expectedPayloadType = cfg_.payloadType;
          return j;
      }()),
      bitrate_(Millis(1000)) {
    DepacketizerConfig dc;
    dc.codec = cfg_.codec;
    dc.clockRate = cfg_.clockRate;
    dc.paramSets = cfg_.paramSets;
    dc.dropUntilKeyframeAfterLoss = cfg_.dropUntilKeyframeAfterLoss;
    depack_ = Depacketizer::create(dc);
    depack_->setSink([this](AccessUnit&& au) {
        ++aus_;
        lastAu_ = au.completedAt;
        if (sink_) sink_(std::move(au));
    });
}

void StreamReceiver::deliver(std::vector<OwnedRtpPacket>& pkts) {
    for (auto& p : pkts) depack_->push(p.sequence, p.timestamp, p.marker, p.payload, p.arrival);
    pkts.clear();
}

void StreamReceiver::onRtp(ByteView packet, TimePoint now) {
    auto v = parseRtp(packet);
    if (!v) {
        ++malformed_;
        return;
    }
    lastPacket_ = now;
    bitrate_.add(now, packet.size());
    jitter_.push(*v, now, scratch_);
    deliver(scratch_);
}

void StreamReceiver::onRtcp(ByteView packet, TimePoint now) {
    ++rtcp_;
    auto info = parseRtcp(packet);
    if (info.bye) bye_ = true;
    if (info.senderReport) {
        const auto& sr = *info.senderReport;
        lastSrMiddle_ = (sr.ntpMsw << 16) | (sr.ntpLsw >> 16);
        lastSrAt_ = now;
    }
}

void StreamReceiver::poll(TimePoint now) {
    jitter_.poll(now, scratch_);
    deliver(scratch_);
}

std::optional<Bytes> StreamReceiver::takeReceiverReport(TimePoint now) {
    if (!lastPacket_) return std::nullopt;  // nothing to report on yet
    if (lastRr_ && now - *lastRr_ < cfg_.receiverReportInterval) return std::nullopt;
    lastRr_ = now;
    std::uint32_t dlsr = 0;
    if (lastSrAt_) {
        double secs = std::chrono::duration<double>(now - *lastSrAt_).count();
        dlsr = static_cast<std::uint32_t>(secs * 65536.0);
    }
    auto block = jitter_.reportBlock(lastSrAt_ ? lastSrMiddle_ : 0, dlsr);
    return buildReceiverReport(cfg_.localSsrc, block, cfg_.cname);
}

StreamSnapshot StreamReceiver::snapshot(TimePoint now) {
    StreamSnapshot s;
    s.rtp = jitter_.stats();
    s.depacketizer = depack_->stats();
    s.bitrateBps = bitrate_.bitsPerSecond(now);
    s.accessUnits = aus_;
    s.malformedPackets = malformed_;
    s.rtcpPackets = rtcp_;
    s.byeReceived = bye_;
    s.lastPacketAt = lastPacket_;
    s.lastAccessUnitAt = lastAu_;
    return s;
}

}  // namespace hb::rtsp
