#pragma once
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "rtsp/depacketizer.hpp"
#include "rtsp/jitter_buffer.hpp"

namespace hb::rtsp {

// Bounded queue of access units between the network thread and the decoder. On overflow the
// reference chain would break whatever we drop, so the queue jumps to the newest keyframe
// (dropOldest) or rejects the newcomer (!dropOldest) and then waits for the next keyframe.
class AccessUnitQueue {
public:
    AccessUnitQueue(std::size_t capacity, bool dropOldest) : capacity_(capacity ? capacity : 1), dropOldest_(dropOldest) {}
    void push(AccessUnit&& au);
    std::optional<AccessUnit> pop();
    std::size_t size() const { return q_.size(); }
    std::uint64_t dropped() const { return dropped_; }
    void clear() { q_.clear(); }

private:
    std::size_t capacity_;
    bool dropOldest_;
    std::deque<AccessUnit> q_;
    bool waitKeyframe_ = false;
    std::uint64_t dropped_ = 0;
};

struct StreamReceiverConfig {
    Codec codec = Codec::H264;
    int payloadType = -1;
    std::uint32_t clockRate = 90000;
    JitterConfig jitter;
    std::vector<Bytes> paramSets;
    bool dropUntilKeyframeAfterLoss = true;
    std::uint32_t localSsrc = 0x48425354;  // "HBST"
    std::string cname = "hbstudio@localhost";
    Millis receiverReportInterval{2000};
};

struct StreamSnapshot {
    RtpStats rtp;
    DepacketizerStats depacketizer;
    double bitrateBps = 0;
    std::uint64_t accessUnits = 0;
    std::uint64_t malformedPackets = 0;   // failed RTP header validation
    std::uint64_t rtcpPackets = 0;
    bool byeReceived = false;
    std::optional<TimePoint> lastPacketAt;
    std::optional<TimePoint> lastAccessUnitAt;
};

// RTP in -> access units out: parse, reorder, depacketize, measure. Single-threaded; the owner
// calls it from the network thread with an injected clock.
class StreamReceiver {
public:
    using AuSink = std::function<void(AccessUnit&&)>;
    explicit StreamReceiver(StreamReceiverConfig cfg);

    void setSink(AuSink s) { sink_ = std::move(s); }
    void onRtp(ByteView packet, TimePoint now);
    void onRtcp(ByteView packet, TimePoint now);
    void poll(TimePoint now);  // gap timeouts
    // RTCP compound packet (RR + SDES) when one is due; also due once media has been seen.
    std::optional<Bytes> takeReceiverReport(TimePoint now);

    StreamSnapshot snapshot(TimePoint now);

private:
    void deliver(std::vector<OwnedRtpPacket>& pkts);

    StreamReceiverConfig cfg_;
    JitterBuffer jitter_;
    std::unique_ptr<Depacketizer> depack_;
    BitrateMeter bitrate_;
    AuSink sink_;
    std::vector<OwnedRtpPacket> scratch_;

    std::uint64_t aus_ = 0, malformed_ = 0, rtcp_ = 0;
    bool bye_ = false;
    std::optional<TimePoint> lastPacket_, lastAu_;
    std::optional<TimePoint> lastRr_;
    std::uint32_t lastSrMiddle_ = 0;
    std::optional<TimePoint> lastSrAt_;
};

}  // namespace hb::rtsp
