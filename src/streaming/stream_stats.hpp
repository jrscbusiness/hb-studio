#pragma once
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

// Per-destination live health numbers for the TRANSMISSÃO cards. Portable and clock-injectable so the
// arithmetic (sliding-window bitrate, drops, history) is unit-tested; StreamSender feeds it from its
// thread and the UI poller reads snapshot() from another, hence the mutex.
namespace hb::streaming {

std::int64_t steadyNowMs();   // std::chrono::steady_clock in ms (default clock)

class StreamStats {
public:
    using NowMs = std::function<std::int64_t()>;
    static constexpr int kHistorySeconds = 30;   // sparkline length

    struct Snapshot {
        int targetKbps = 0;        // configured video+audio target
        int encoderKbps = 0;       // what the encoder is currently asked for (differs from target under ABR)
        double actualKbps = 0;     // sent bits over the sliding window (video+audio payload, no headers)
        std::uint64_t sentBytes = 0;
        std::uint64_t droppedVideo = 0, droppedAudio = 0;   // frames discarded by the bounded queues (total)
        unsigned droppedRecent = 0;                         // video+audio drops inside the window
        unsigned reconnects = 0;
        int queueVideo = 0, queueAudio = 0;                 // current depth
        std::string lastError;
        std::string encoderName;
        std::vector<int> history;                           // kbps per second, oldest first, <= kHistorySeconds
    };

    explicit StreamStats(NowMs now = nullptr, int windowSeconds = 10);

    void setTargetKbps(int kbps);
    void setEncoderKbps(int kbps);
    void setEncoderName(const std::string& name);
    void onSent(bool video, std::size_t bytes);
    void onDropped(bool video, unsigned frames = 1);
    void onReconnect();
    void setQueueDepth(int video, int audio);
    void setLastError(const std::string& err);
    void reset();   // new run: clears counters/history, keeps nothing

    Snapshot snapshot() const;

private:
    struct Bucket {
        std::int64_t sec = 0;
        std::uint64_t bytes = 0;
        unsigned drops = 0;
    };
    Bucket& bucketFor(std::int64_t sec);   // mutex held
    std::int64_t nowSec() const;

    NowMs now_;
    int window_;
    mutable std::mutex m_;
    std::deque<Bucket> buckets_;
    std::int64_t firstSec_ = -1;
    Snapshot s_;
};

// {"targetKbps":..,"encoderKbps":..,"actualKbps":..,"droppedV":..,"droppedA":..,"droppedRecent":..,
//  "reconnects":..,"qV":..,"qA":..,"err":"..","encoder":"..","hist":[..]}
std::string statsToJson(const StreamStats::Snapshot& s);

}  // namespace hb::streaming
