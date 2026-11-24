#pragma once
// Pure (no I/O, no threads, no clocks) synchronisation logic of the multitrack recorder.
//
// Every ISO track is stamped from ONE monotonic session clock at capture time (nanoseconds since the
// session start, see session_clock.hpp). These classes turn those stamps into positions on a constant
// timeline: video slots (constant frame rate) and audio sample indexes (sample accurate), and say what the
// writer must do to keep the file on that timeline: repeat a frame, skip a frame, insert silence, drop or
// repeat a few samples to absorb long-session clock drift. See docs/GRAVACAO_MULTIPISTA.md.
#include <cstdint>

namespace hb::multitrack {

using Ns = std::int64_t;
inline constexpr Ns kNsPerSec = 1000000000;
inline constexpr Ns kNsPerMs = 1000000;

// Overflow-safe exact arithmetic for "units" at the rate num/den per second (frames at 30000/1001, samples
// at 48000/1). Rounds to nearest, halves away from zero. Valid for |ns| up to several decades.
std::int64_t nsToUnits(Ns ns, std::uint64_t num, std::uint64_t den);
Ns unitsToNs(std::uint64_t units, std::uint64_t num, std::uint64_t den);

// ------------------------------------------------------------------------------------------------ video
struct VideoTimelineConfig {
    int fpsNum = 30, fpsDen = 1;
    // A gap up to this many slots is filled by repeating the previous frame (keeps a constant frame rate);
    // a longer gap is jumped (the pts leaps, nothing is written for the missing slots). 0 = two seconds.
    std::uint32_t maxRepeatSlots = 0;
};

struct VideoPlacement {
    bool drop = false;                  // frame arrived for a slot that is already filled: do not write it
    std::uint64_t slot = 0;             // absolute slot (from the session start) of this frame
    std::uint64_t repeatFrom = 0;       // first slot to fill by repeating the previous frame
    std::uint64_t repeatCount = 0;      // how many consecutive slots to fill with the previous frame
    std::uint64_t jumpedSlots = 0;      // slots between the repeats and `slot` that stay empty (long stall)
};

class VideoTimeline {
public:
    explicit VideoTimeline(VideoTimelineConfig cfg = {});

    // `captureNs`: session time at which the frame was captured/sampled.
    VideoPlacement onFrame(Ns captureNs);

    bool started() const { return started_; }
    std::uint64_t firstSlot() const { return firstSlot_; }
    std::uint64_t nextSlot() const { return next_; }
    Ns slotNs(std::uint64_t slot) const;                  // exact start time of a slot
    std::uint64_t slotAt(Ns ns) const;                    // nearest slot (clamped at 0)

    std::uint64_t written() const { return written_; }    // frames accepted (not counting repeats)
    std::uint64_t repeated() const { return repeated_; }  // slots filled by repetition
    std::uint64_t dropped() const { return dropped_; }    // frames refused (arrived for a filled slot)
    std::uint64_t jumped() const { return jumped_; }      // slots skipped by long stalls
    std::uint64_t worstGapSlots() const { return worstGap_; }

private:
    VideoTimelineConfig cfg_;
    std::uint32_t maxRepeat_;
    bool started_ = false;
    std::uint64_t firstSlot_ = 0, next_ = 0;
    std::uint64_t written_ = 0, repeated_ = 0, dropped_ = 0, jumped_ = 0, worstGap_ = 0;
};

// ------------------------------------------------------------------------------------------------ audio
struct AudioTimelineConfig {
    int sampleRate = 48000;
    // A block that starts further than this from where the timeline expects it is a real gap/overlap:
    // fixed at once (silence / skip). 200 ms = beyond what an engine xrun or scheduling hiccup produces.
    Ns hardGapNs = 200 * kNsPerMs;
    // Below the hard gap the stamps are jittery (they are taken when a block ARRIVES), so only the minimum
    // offset seen over `windowBlocks` blocks is trusted; when it exceeds this tolerance the offset is
    // corrected GRADUALLY (a few samples per block) instead of with an audible gap.
    Ns toleranceNs = 2 * kNsPerMs;
    int windowBlocks = 100;                  // 1 s of 10 ms blocks
    std::uint32_t maxCorrectionPerBlock = 8; // samples repeated/dropped per block while correcting
};

struct AudioPlacement {
    std::uint64_t skipFrames = 0;     // drop this many frames from the START of the block
    std::uint64_t silenceBefore = 0;  // write this many frames of silence BEFORE the (remaining) block
    std::uint64_t repeatLast = 0;     // write this many copies of the last written frame BEFORE the block
};

class AudioTimeline {
public:
    explicit AudioTimeline(AudioTimelineConfig cfg = {});

    // `startNs`: session time of the FIRST sample of the block. The first block is aligned exactly: leading
    // silence (or a trim) so that sample 0 of the file is the session start.
    AudioPlacement onBlock(Ns startNs, std::uint64_t frames);
    // The caller lost `frames` sample frames (queue overflow) and replaced them with silence.
    void addExternalGap(std::uint64_t frames);

    std::uint64_t position() const { return pos_; }   // frames the file holds (or will hold) so far
    Ns positionNs() const { return unitsToNs(pos_, static_cast<std::uint64_t>(cfg_.sampleRate), 1); }

    std::uint64_t leadingSilence() const { return leading_; }
    std::uint64_t insertedSilence() const { return silence_; }   // hard gaps + queue overflow (excl. leading)
    std::uint64_t skippedFrames() const { return skipped_; }
    std::uint64_t repeatedFrames() const { return repeated_; }
    // Gradual corrections only: frames added minus frames dropped.
    std::int64_t netCorrectionFrames() const {
        return static_cast<std::int64_t>(repeated_) - static_cast<std::int64_t>(skippedSoft_);
    }
    Ns lastOffsetNs() const { return lastOffset_; }
    // Average rate error of the source clock relative to the session clock (+ = the source runs SLOW, so
    // samples had to be added), from the gradual corrections over `elapsedNs`.
    double driftPpm(Ns elapsedNs) const;

private:
    AudioTimelineConfig cfg_;
    bool first_ = true;
    std::uint64_t pos_ = 0;
    std::uint64_t leading_ = 0, silence_ = 0, skipped_ = 0, skippedSoft_ = 0, repeated_ = 0;
    std::int64_t owed_ = 0;          // gradual correction still to apply (+ add, - drop), in frames
    Ns windowMin_ = 0;
    int windowCount_ = 0;
    Ns lastOffset_ = 0;
};

}  // namespace hb::multitrack
