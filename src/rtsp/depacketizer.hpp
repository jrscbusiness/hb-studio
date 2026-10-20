#pragma once
#include <functional>
#include <memory>
#include <vector>

#include "rtsp/types.hpp"

namespace hb::rtsp {

struct DepacketizerConfig {
    Codec codec = Codec::H264;
    std::uint32_t clockRate = 90000;
    // Out-of-band parameter sets (from SDP): H.264 SPS,PPS / H.265 VPS,SPS,PPS (raw NAL, no start code).
    std::vector<Bytes> paramSets;
    // After packet loss the reference chain is broken: drop everything until the next clean
    // keyframe (default). If false, damaged AUs are emitted with AccessUnit::damaged = true.
    bool dropUntilKeyframeAfterLoss = true;
    std::size_t maxAuBytes = 8u * 1024 * 1024;
};

struct DepacketizerStats {
    std::uint64_t packets = 0;
    std::uint64_t accessUnits = 0;        // emitted
    std::uint64_t keyframes = 0;          // emitted keyframes
    std::uint64_t damagedAus = 0;         // AUs that had missing packets (emitted or not)
    std::uint64_t droppedAus = 0;         // AUs not emitted (damaged / waiting for keyframe / oversize)
    std::uint64_t seqGaps = 0;            // forward jumps in sequence numbers seen
    std::uint64_t oldOrDuplicate = 0;     // packets discarded as older than expected
    std::uint64_t truncatedPayloads = 0;  // malformed / truncated RTP payloads
    std::uint64_t discardedFragments = 0; // FU reassemblies abandoned
    std::uint64_t unsupportedNals = 0;    // STAP-B/MTAP/FU-B/PACI/etc.
    std::uint64_t paramSetInjections = 0; // AUs where SPS/PPS(/VPS) were prepended
    std::uint64_t oversizeAus = 0;
};

// Turns an in-order stream of RTP payloads into Annex B access units. Feed it packets from the
// jitter buffer; it additionally defends itself against duplicates, reordering of old packets
// and truncated payloads. It never throws on hostile input.
class Depacketizer {
public:
    using Sink = std::function<void(AccessUnit&&)>;

    static std::unique_ptr<Depacketizer> create(const DepacketizerConfig& cfg);
    virtual ~Depacketizer() = default;

    void setSink(Sink s) { sink_ = std::move(s); }
    void push(std::uint16_t seq, std::uint32_t timestamp, bool marker, ByteView payload, TimePoint arrival);
    // Emit whatever is pending (e.g. when the stream stalls and the marker bit was lost).
    void flush(TimePoint now);
    void reset();
    const DepacketizerStats& stats() const { return stats_; }
    bool waitingForKeyframe() const { return waitingKeyframe_; }

protected:
    explicit Depacketizer(const DepacketizerConfig& cfg);

    virtual void parsePayload(ByteView payload) = 0;
    virtual bool isKeyNal(const std::uint8_t* nal, std::size_t n) const = 0;
    virtual void noteNal(const std::uint8_t* nal, std::size_t n) = 0;   // track param sets / AUD
    virtual void prependParams(Bytes& out, std::size_t at) = 0;          // inject missing params
    virtual void resetCodecState() = 0;
    virtual void onLoss() {}                                             // abandon fragment state

    // Helpers for subclasses.
    void addNal(const std::uint8_t* nal, std::size_t n);
    std::size_t beginNal();                 // appends start code, returns offset of NAL start
    void appendNalBytes(const std::uint8_t* d, std::size_t n);
    void abortNalAt(std::size_t startCodeOffset);
    void completeNal(std::size_t startCodeOffset);  // classify + count a finished NAL
    void markDamaged() { cur_.damaged = true; }
    bool auOverflow() const { return cur_.data.size() > cfg_.maxAuBytes; }

    DepacketizerConfig cfg_;
    DepacketizerStats stats_;
    AccessUnit cur_;
    bool curKey_ = false;
    bool hasSps_ = false, hasPps_ = false, hasVps_ = false;
    std::size_t auPrefixEnd_ = 0;  // end of a leading AUD NAL (params go after it)
    std::size_t nalCount_ = 0;

private:
    void startAuIfNeeded(std::uint32_t ts, bool lossBefore);
    void emitCurrent(TimePoint t);
    void clearCurrent();
    std::int64_t ptsFor(std::uint32_t ts);

    Sink sink_;
    bool auOpen_ = false;
    bool haveSeq_ = false;
    std::uint16_t expectedSeq_ = 0;
    bool pendingLoss_ = false;
    bool waitingKeyframe_ = true;
    bool haveTs_ = false;
    std::uint32_t lastTs_ = 0;
    std::int64_t ptsTicks_ = 0;
    bool discardAu_ = false;
    std::uint32_t curTs_ = 0;
    TimePoint lastArrival_{};
};

}  // namespace hb::rtsp
