#include "streaming/stream_stats.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>

namespace hb::streaming {

std::int64_t steadyNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

StreamStats::StreamStats(NowMs now, int windowSeconds)
    : now_(now ? std::move(now) : NowMs(steadyNowMs)), window_(std::clamp(windowSeconds, 1, kHistorySeconds)) {}

std::int64_t StreamStats::nowSec() const { return now_() / 1000; }

StreamStats::Bucket& StreamStats::bucketFor(std::int64_t sec) {
    if (firstSec_ < 0) firstSec_ = sec;
    if (buckets_.empty() || buckets_.back().sec < sec) {
        Bucket b;
        b.sec = sec;
        buckets_.push_back(b);
        while (!buckets_.empty() && buckets_.front().sec < sec - kHistorySeconds - 1) buckets_.pop_front();
    }
    return buckets_.back();
}

void StreamStats::setTargetKbps(int kbps) {
    std::lock_guard lock(m_);
    s_.targetKbps = kbps;
    if (s_.encoderKbps == 0) s_.encoderKbps = kbps;
}
void StreamStats::setEncoderKbps(int kbps) {
    std::lock_guard lock(m_);
    s_.encoderKbps = kbps;
}
void StreamStats::setEncoderName(const std::string& name) {
    std::lock_guard lock(m_);
    s_.encoderName = name;
}

void StreamStats::onSent(bool /*video*/, std::size_t bytes) {
    const std::int64_t sec = nowSec();
    std::lock_guard lock(m_);
    bucketFor(sec).bytes += bytes;
    s_.sentBytes += bytes;
}

void StreamStats::onDropped(bool video, unsigned frames) {
    const std::int64_t sec = nowSec();
    std::lock_guard lock(m_);
    bucketFor(sec).drops += frames;
    (video ? s_.droppedVideo : s_.droppedAudio) += frames;
}

void StreamStats::onReconnect() {
    std::lock_guard lock(m_);
    ++s_.reconnects;
}

void StreamStats::setQueueDepth(int video, int audio) {
    std::lock_guard lock(m_);
    s_.queueVideo = video;
    s_.queueAudio = audio;
}

void StreamStats::setLastError(const std::string& err) {
    std::lock_guard lock(m_);
    s_.lastError = err;
}

void StreamStats::reset() {
    std::lock_guard lock(m_);
    buckets_.clear();
    firstSec_ = -1;
    s_ = Snapshot{};
}

StreamStats::Snapshot StreamStats::snapshot() const {
    const std::int64_t nowMs = now_();
    const std::int64_t cur = nowMs / 1000;
    std::lock_guard lock(m_);
    Snapshot out = s_;
    auto bytesAt = [&](std::int64_t sec, unsigned* drops) -> std::uint64_t {
        for (const auto& b : buckets_)
            if (b.sec == sec) {
                if (drops) *drops += b.drops;
                return b.bytes;
            }
        return 0;
    };
    // Window = the last `window_` COMPLETE seconds (the current second is still filling up). Before
    // that much history exists, divide only by the seconds that have elapsed since the first sample.
    if (firstSec_ >= 0) {
        const std::int64_t from = std::max(cur - window_, firstSec_);
        std::uint64_t total = 0;
        unsigned drops = 0;
        for (std::int64_t s = from; s < cur; ++s) total += bytesAt(s, &drops);
        drops += 0;
        const std::int64_t span = cur - from;
        if (span >= 1) {
            out.actualKbps = static_cast<double>(total) * 8.0 / 1000.0 / static_cast<double>(span);
        } else {
            // first (partial) second: extrapolate from what we have, never below a 1 s floor
            const std::int64_t elapsed = std::max<std::int64_t>(1000, nowMs - firstSec_ * 1000);
            out.actualKbps = static_cast<double>(bytesAt(cur, nullptr)) * 8.0 / static_cast<double>(elapsed);
        }
        out.droppedRecent = drops;
        unsigned curDrops = 0;
        bytesAt(cur, &curDrops);
        out.droppedRecent += curDrops;
        const std::int64_t hFrom = std::max(cur - kHistorySeconds, firstSec_);
        for (std::int64_t s = hFrom; s < cur; ++s)
            out.history.push_back(static_cast<int>(static_cast<double>(bytesAt(s, nullptr)) * 8.0 / 1000.0 + 0.5));
    }
    return out;
}

namespace {
std::string esc(const std::string& in) {
    std::string o;
    for (unsigned char c : in) {
        if (c == '"' || c == '\\') { o += '\\'; o += static_cast<char>(c); }
        else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += static_cast<char>(c);
    }
    return o;
}
}  // namespace

std::string statsToJson(const StreamStats::Snapshot& s) {
    char num[32];
    std::snprintf(num, sizeof num, "%.1f", s.actualKbps);
    std::string j = "{\"targetKbps\":" + std::to_string(s.targetKbps) + ",\"encoderKbps\":" + std::to_string(s.encoderKbps) +
                    ",\"actualKbps\":" + num + ",\"droppedV\":" + std::to_string(s.droppedVideo) +
                    ",\"droppedA\":" + std::to_string(s.droppedAudio) + ",\"droppedRecent\":" + std::to_string(s.droppedRecent) +
                    ",\"reconnects\":" + std::to_string(s.reconnects) + ",\"qV\":" + std::to_string(s.queueVideo) +
                    ",\"qA\":" + std::to_string(s.queueAudio) + ",\"err\":\"" + esc(s.lastError) + "\",\"encoder\":\"" +
                    esc(s.encoderName) + "\",\"hist\":[";
    for (std::size_t i = 0; i < s.history.size(); ++i) j += (i ? "," : "") + std::to_string(s.history[i]);
    j += "]}";
    return j;
}

}  // namespace hb::streaming
