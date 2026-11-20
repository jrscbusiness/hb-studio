#pragma once
// Common machinery of CaptureStream and PlaybackStream: a supervised OS audio stream that
// (re)opens itself with exponential backoff and reports a human readable state.
//
//   Idle -> Connecting -> Online <-> NoSignal
//                \-> Error (message, retry with backoff) -> Connecting ...
//
// Nothing here blocks the caller for long, throws, or runs on the OS audio thread except the
// callbacks supplied by the derived classes. tick() is the supervisor step: a private thread
// calls it every 50 ms, tests call it by hand with an injected clock.
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "audio_io/raw_device.hpp"

namespace hb::audio_io {

enum class StreamState { Idle, Connecting, Online, NoSignal, Error };
const char* streamStateName(StreamState s) noexcept;  // "idle" "connecting" "online" "nosignal" "error"

struct StreamOptions {
    Backend backend = Backend::Auto;
    DeviceOpener opener;                      // empty = miniaudio
    std::function<std::int64_t()> clockNs;    // empty = steady clock
    bool supervisor = true;                   // false: the caller drives tick()
    int periodMs = 10;
    int noSignalMs = 1500;                    // no audio callbacks for this long => NoSignal
    int backoffInitialMs = 250;
    int backoffMaxMs = 4000;
};

struct StreamStatus {
    StreamState state = StreamState::Idle;
    std::string message;          // pt-BR, ready to show
    std::string deviceId;
    int sampleRate = 0;           // native rate of the open device (0 when closed)
    int nativeChannels = 0;
    std::uint32_t opens = 0;      // successful opens (reopens = opens - 1)
    std::uint64_t frames = 0;     // frames captured / rendered since start
    std::uint64_t underruns = 0;  // ring underruns (consumer starved)
    std::uint64_t overruns = 0;   // ring overruns (producer dropped)
};

class DeviceStream {
public:
    virtual ~DeviceStream();
    DeviceStream(const DeviceStream&) = delete;
    DeviceStream& operator=(const DeviceStream&) = delete;

    void start();  // idempotent
    void stop();   // closes the device; idempotent
    void tick();   // supervisor step (safe from any thread, serialised)
    StreamStatus status() const;
    const std::string& deviceId() const noexcept { return deviceId_; }
    DeviceKind kind() const noexcept { return kind_; }

protected:
    DeviceStream(DeviceKind kind, std::string deviceId, StreamOptions options);

    // Called under the supervisor lock, after a device was opened and before it starts. Returns the
    // callbacks to install.
    virtual RawCallbacks onOpened(int sampleRate) = 0;
    virtual void onClosed() {}
    virtual void fillCounters(StreamStatus& s) const { (void)s; }

    // From the OS audio thread: records liveness.
    void noteData(std::size_t frames) noexcept;
    std::int64_t nowNs() const;
    // Derived destructors call this first, so no callback outlives the derived members.
    void shutdown();

private:
    void attemptOpen(std::int64_t now);
    void closeDevice();
    void setStatus(StreamState st, std::string message);
    void loop();

    DeviceKind kind_;
    std::string deviceId_;
    StreamOptions opt_;

    std::mutex tickMutex_;                 // serialises tick/start/stop (may be held while opening)
    bool running_ = false;
    std::unique_ptr<RawDevice> dev_;
    std::int64_t nextAttempt_ = 0, openedAt_ = 0;
    int backoffMs_ = 0;
    std::uint32_t opens_ = 0;
    int rate_ = 0, chans_ = 0;

    mutable std::mutex statusMutex_;       // short critical sections only
    StreamState state_ = StreamState::Idle;
    std::string message_;

    std::atomic<std::int64_t> lastData_{INT64_MIN};
    std::atomic<std::uint64_t> frames_{0};

    std::thread thread_;
    std::mutex wakeMutex_;
    std::condition_variable wake_;
    bool stopThread_ = false;
};

}  // namespace hb::audio_io
