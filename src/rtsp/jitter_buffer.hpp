#pragma once
#include <deque>
#include <map>
#include <optional>
#include <vector>

#include "core/hardware_profile.hpp"
#include "rtsp/rtp.hpp"
#include "rtsp/types.hpp"

namespace hb::rtsp {

// An RTP packet owned by the buffer.
struct OwnedRtpPacket {
    std::uint8_t payloadType = 0;
    bool marker = false;
    std::uint16_t sequence = 0;
    std::uint32_t timestamp = 0;
    std::uint32_t ssrc = 0;
    Bytes payload;
    TimePoint arrival{};
};

struct JitterConfig {
    Millis maxReorderWait{200};  // how long to wait for a missing packet before declaring it lost
    std::size_t maxPackets = 512;  // capacity of the out-of-order store
    // When the store is full while waiting for a gap: true = give up on the gap now (release the
    // oldest); false = discard the newly arrived packet and keep waiting.
    bool dropOldestOnOverflow = true;
    std::uint32_t clockRate = 90000;
    int expectedPayloadType = -1;  // -1 = accept any
    std::uint16_t maxDropout = 3000;  // forward jump treated as a stream restart (RFC 3550 A.1)
    std::uint16_t maxMisorder = 100;

    // Derived from hb::core::latencyParams. TCP interleaved cannot reorder, so the gap wait is
    // capped at a few ms there (a gap on TCP means the sender dropped the packet).
    static JitterConfig forMode(core::LatencyMode mode, bool inOrderTransport, std::uint32_t clockRate = 90000);
};

// Reception statistics per RFC 3550 appendix A.
struct RtpStats {
    std::uint64_t received = 0;
    std::uint64_t bytes = 0;
    std::uint64_t expected = 0;
    std::uint64_t lost = 0;          // cumulative, never negative here
    std::uint64_t duplicates = 0;
    std::uint64_t late = 0;          // arrived after its gap had been given up on
    std::uint64_t reordered = 0;     // arrived out of order but in time
    std::uint64_t overflowDrops = 0;
    std::uint64_t foreign = 0;       // wrong payload type / SSRC
    std::uint64_t resyncs = 0;
    std::uint32_t jitter = 0;        // interarrival jitter in RTP clock units
    std::uint32_t extHighestSeq = 0;
    double lossPercent = 0;
};

// Sliding-window bitrate meter over injected time.
class BitrateMeter {
public:
    explicit BitrateMeter(Millis window = Millis(1000)) : window_(window) {}
    void add(TimePoint t, std::size_t bytes);
    double bitsPerSecond(TimePoint now);

private:
    void trim(TimePoint now);
    Millis window_;
    std::deque<std::pair<TimePoint, std::size_t>> samples_;
    std::size_t sum_ = 0;
};

// Reorders by RTP sequence number (with wraparound), removes duplicates and bounds the delay
// added to the stream. Contiguous packets are released immediately; only a gap makes it wait.
class JitterBuffer {
public:
    explicit JitterBuffer(JitterConfig cfg = {});

    // Inserts a packet and appends everything now deliverable (in order) to `out`.
    void push(const RtpPacketView& pkt, TimePoint now, std::vector<OwnedRtpPacket>& out);
    // Gives up on gaps older than maxReorderWait. Call periodically.
    void poll(TimePoint now, std::vector<OwnedRtpPacket>& out);
    void reset();

    const RtpStats& stats();
    std::size_t pending() const { return store_.size(); }
    const JitterConfig& config() const { return cfg_; }

    // Values for an RTCP receiver report. fractionLost covers the interval since the last call.
    ReportBlock reportBlock(std::uint32_t lastSr, std::uint32_t delaySinceSr);
    std::uint32_t ssrc() const { return ssrc_; }

private:
    void drain(TimePoint now, std::vector<OwnedRtpPacket>& out);
    void updateJitter(const RtpPacketView& pkt, TimePoint now);
    void computeDerived();

    JitterConfig cfg_;
    RtpStats stats_;

    bool started_ = false;
    std::uint32_t ssrc_ = 0;
    std::uint32_t candidateSsrc_ = 0;
    int candidateCount_ = 0;
    std::uint16_t maxSeq_ = 0;
    std::uint32_t cycles_ = 0;      // multiples of 65536
    std::int64_t baseExt_ = 0;
    std::int64_t nextExt_ = 0;      // next sequence to release
    std::int64_t maxExt_ = 0;
    std::uint64_t releasedMask_ = 0;  // bit i: (nextExt_-1-i) was released
    bool haveBadSeq_ = false;
    std::uint16_t badSeq_ = 0;
    std::map<std::int64_t, OwnedRtpPacket> store_;
    std::optional<TimePoint> gapSince_;

    std::int64_t skipped_ = 0;       // packets excluded from 'expected' after resyncs
    std::int64_t restartExt_ = 0;
    bool haveRestart_ = false;

    // jitter state
    bool haveTransit_ = false;
    double lastArrival_ = 0;
    std::uint32_t lastTs_ = 0;
    double jitter_ = 0;

    // report interval bookkeeping
    std::uint64_t prevExpected_ = 0, prevReceived_ = 0;
};

}  // namespace hb::rtsp
