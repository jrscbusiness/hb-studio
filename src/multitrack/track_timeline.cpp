#include "multitrack/track_timeline.hpp"

#include <algorithm>
#include <limits>

namespace hb::multitrack {

std::int64_t nsToUnits(Ns ns, std::uint64_t num, std::uint64_t den) {
    if (num == 0 || den == 0) return 0;
    const bool neg = ns < 0;
    const std::uint64_t a = neg ? static_cast<std::uint64_t>(-(ns + 1)) + 1u : static_cast<std::uint64_t>(ns);
    const std::uint64_t whole = a / 1000000000ull, rem = a % 1000000000ull;
    const std::uint64_t A = whole * num;
    const std::uint64_t q = A / den, r = A % den;
    const std::uint64_t d = den * 1000000000ull;
    const std::uint64_t frac = r * 1000000000ull + rem * num;   // value = q + frac / d
    const std::uint64_t u = q + (2 * frac + d) / (2 * d);       // round half up (on the magnitude)
    return neg ? -static_cast<std::int64_t>(u) : static_cast<std::int64_t>(u);
}

Ns unitsToNs(std::uint64_t units, std::uint64_t num, std::uint64_t den) {
    if (num == 0) return 0;
    const std::uint64_t q = units / num, r = units % num;
    const std::uint64_t whole = q * den * 1000000000ull;
    const std::uint64_t part = (r * den * 1000000000ull + num / 2) / num;
    return static_cast<Ns>(whole + part);
}

// ------------------------------------------------------------------------------------------------ video
VideoTimeline::VideoTimeline(VideoTimelineConfig cfg) : cfg_(cfg) {
    if (cfg_.fpsNum <= 0) cfg_.fpsNum = 30;
    if (cfg_.fpsDen <= 0) cfg_.fpsDen = 1;
    maxRepeat_ = cfg_.maxRepeatSlots ? cfg_.maxRepeatSlots
                                     : static_cast<std::uint32_t>(std::max(1, 2 * cfg_.fpsNum / cfg_.fpsDen));
}

Ns VideoTimeline::slotNs(std::uint64_t slot) const {
    return unitsToNs(slot, static_cast<std::uint64_t>(cfg_.fpsNum), static_cast<std::uint64_t>(cfg_.fpsDen));
}

std::uint64_t VideoTimeline::slotAt(Ns ns) const {
    const std::int64_t s =
        nsToUnits(ns, static_cast<std::uint64_t>(cfg_.fpsNum), static_cast<std::uint64_t>(cfg_.fpsDen));
    return s < 0 ? 0 : static_cast<std::uint64_t>(s);
}

VideoPlacement VideoTimeline::onFrame(Ns captureNs) {
    VideoPlacement p;
    const std::uint64_t s = slotAt(captureNs);
    if (!started_) {
        started_ = true;
        firstSlot_ = s;
        next_ = s + 1;
        p.slot = s;
        ++written_;
        return p;
    }
    if (s < next_) {  // too early / duplicate: the slot is already taken
        p.drop = true;
        p.slot = s;
        ++dropped_;
        return p;
    }
    const std::uint64_t gap = s - next_;
    p.slot = s;
    if (gap > 0) {
        p.repeatFrom = next_;
        p.repeatCount = std::min<std::uint64_t>(gap, maxRepeat_);
        p.jumpedSlots = gap - p.repeatCount;
        repeated_ += p.repeatCount;
        jumped_ += p.jumpedSlots;
        worstGap_ = std::max(worstGap_, gap);
    }
    next_ = s + 1;
    ++written_;
    return p;
}

// ------------------------------------------------------------------------------------------------ audio
AudioTimeline::AudioTimeline(AudioTimelineConfig cfg) : cfg_(cfg) {
    if (cfg_.sampleRate <= 0) cfg_.sampleRate = 48000;
    cfg_.windowBlocks = std::max(1, cfg_.windowBlocks);
    cfg_.maxCorrectionPerBlock = std::max<std::uint32_t>(1, cfg_.maxCorrectionPerBlock);
    windowMin_ = std::numeric_limits<Ns>::max();
}

void AudioTimeline::addExternalGap(std::uint64_t frames) {
    pos_ += frames;
    silence_ += frames;
}

AudioPlacement AudioTimeline::onBlock(Ns startNs, std::uint64_t frames) {
    AudioPlacement p;
    const std::uint64_t rate = static_cast<std::uint64_t>(cfg_.sampleRate);
    if (frames == 0) return p;

    if (first_) {
        const std::int64_t off = nsToUnits(startNs, rate, 1);
        if (off > 0) {
            p.silenceBefore = static_cast<std::uint64_t>(off);
            leading_ = p.silenceBefore;
            first_ = false;
        } else if (off < 0) {
            p.skipFrames = std::min<std::uint64_t>(frames, static_cast<std::uint64_t>(-off));
            skipped_ += p.skipFrames;
            if (p.skipFrames >= frames) return p;  // the whole block predates the session: wait for the next
            first_ = false;
        } else {
            first_ = false;
        }
        pos_ += p.silenceBefore + (frames - p.skipFrames);
        return p;
    }

    const Ns expected = unitsToNs(pos_, rate, 1);
    const Ns d = startNs - expected;  // + : the block starts LATER than the timeline expects
    lastOffset_ = d;

    if (d > cfg_.hardGapNs || d < -cfg_.hardGapNs) {
        owed_ = 0;
        windowCount_ = 0;
        windowMin_ = std::numeric_limits<Ns>::max();
        const std::int64_t f = nsToUnits(d, rate, 1);
        if (f > 0) {
            p.silenceBefore = static_cast<std::uint64_t>(f);
            silence_ += p.silenceBefore;
        } else {
            p.skipFrames = std::min<std::uint64_t>(frames, static_cast<std::uint64_t>(-f));
            skipped_ += p.skipFrames;
        }
    } else {
        windowMin_ = std::min(windowMin_, d);
        if (++windowCount_ >= cfg_.windowBlocks) {
            // Jitter only ever delays a block, so the minimum is the best estimate of the real offset.
            if (windowMin_ > cfg_.toleranceNs || windowMin_ < -cfg_.toleranceNs)
                owed_ = nsToUnits(windowMin_, rate, 1);
            else
                owed_ = 0;
            windowCount_ = 0;
            windowMin_ = std::numeric_limits<Ns>::max();
        }
        if (owed_ > 0) {
            const std::uint64_t n =
                std::min<std::uint64_t>(static_cast<std::uint64_t>(owed_), cfg_.maxCorrectionPerBlock);
            p.repeatLast = n;
            repeated_ += n;
            owed_ -= static_cast<std::int64_t>(n);
        } else if (owed_ < 0) {
            const std::uint64_t n = std::min<std::uint64_t>(
                {static_cast<std::uint64_t>(-owed_), cfg_.maxCorrectionPerBlock, frames - 1});
            p.skipFrames = n;
            skipped_ += n;
            skippedSoft_ += n;
            owed_ += static_cast<std::int64_t>(n);
        }
    }
    pos_ += p.silenceBefore + p.repeatLast + (frames - p.skipFrames);
    return p;
}

double AudioTimeline::driftPpm(Ns elapsedNs) const {
    if (elapsedNs <= 0) return 0.0;
    const double net = static_cast<double>(netCorrectionFrames());
    const double secs = static_cast<double>(elapsedNs) / 1e9;
    return net / (secs * cfg_.sampleRate) * 1e6;
}

}  // namespace hb::multitrack
