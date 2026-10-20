#pragma once
// Operational-safety wrapper around a PTZProvider:
//  - command rate limiter (<= 20 Hz, hard cap),
//  - guaranteed Stop: movement must be renewed within `watchdog`, otherwise Stop is sent
//    (also on disconnect),
//  - non-blocking calls: public methods only record intent / enqueue; the actual I/O
//    happens in tick(), called periodically by the PTZ engine thread (suggested 100 Hz),
//  - injectable clock.
// A PTZ failure never reaches video: errors are reported through the callback only.
#include <deque>
#include <functional>
#include <memory>
#include <mutex>

#include "ptz/clock.hpp"
#include "ptz/controller.hpp"

namespace hb::ptz {

struct GuardConfig {
    double maxRateHz = 20.0;       // clamped to [1, 20]
    Millis watchdog{500};          // movement not renewed within this time -> Stop
    std::size_t maxQueue = 32;     // queued non-movement commands (presets, iris, ...)
    Millis statusPoll{1000};       // 0 = never poll status
};

struct GuardStats {
    std::uint64_t sent = 0;
    std::uint64_t failed = 0;
    std::uint64_t watchdogStops = 0;
    std::uint64_t droppedQueueFull = 0;
};

class GuardedController : public PTZController {
public:
    using ErrorCallback = std::function<void(const std::string& op, const PtzResult&)>;

    GuardedController(std::unique_ptr<PTZProvider> provider, std::shared_ptr<IPtzClock> clock,
                      GuardConfig cfg = {}, ErrorCallback onError = {});

    // Drives all pending work. Safe to call from one engine thread while the UI calls
    // the public methods from another.
    void tick();

    GuardStats stats() const;
    std::string providerName() const { return provider_->name(); }

    const PTZCapabilities& capabilities() const override { return provider_->capabilities(); }

    PtzResult connect() override;
    PtzResult disconnect() override;  // sends Stop first (bypassing the rate limiter)

    // Movement: records intent and renews the watchdog. Call repeatedly while a control is held.
    PtzResult panLeft() override { return setDir(Axis::Pan, -1); }
    PtzResult panRight() override { return setDir(Axis::Pan, 1); }
    PtzResult tiltUp() override { return setDir(Axis::Tilt, 1); }
    PtzResult tiltDown() override { return setDir(Axis::Tilt, -1); }
    PtzResult zoomIn() override { return setDir(Axis::Zoom, 1); }
    PtzResult zoomOut() override { return setDir(Axis::Zoom, -1); }
    PtzResult stop() override;
    PtzResult setPanSpeed(double s) override;
    PtzResult setTiltSpeed(double s) override;
    PtzResult setZoomSpeed(double s) override;

    PtzResult gotoPreset(int id) override;
    PtzResult savePreset(int id) override;
    PtzResult deletePreset(int id) override;
    PtzReply<std::vector<PresetInfo>> getPresets() override;  // cached; refreshed by tick()

    PtzResult focusAuto() override;
    PtzResult focusManual() override;
    PtzResult focusNear() override;
    PtzResult focusFar() override;
    PtzResult irisOpen() override;
    PtzResult irisClose() override;

    PtzReply<CameraStatus> getCameraStatus() override;  // cached; refreshed by tick()
    PtzResult moveAbsolute(const PtzPosition& p) override;

private:
    enum class Axis { Pan, Tilt, Zoom };
    struct Op {
        std::string name;
        std::function<PtzResult(PTZProvider&)> run;
    };

    PtzResult setDir(Axis a, int dir);
    PtzResult enqueue(const char* name, bool supported, std::function<PtzResult(PTZProvider&)> run);
    void report(const std::string& op, const PtzResult& r);
    MoveVector vector() const;
    void sendStop(TimePoint now);  // requires no lock held

    std::unique_ptr<PTZProvider> provider_;
    std::shared_ptr<IPtzClock> clock_;
    GuardConfig cfg_;
    ErrorCallback onError_;
    Millis minInterval_;

    mutable std::mutex mu_;
    bool connected_ = false;
    int panDir_ = 0, tiltDir_ = 0, zoomDir_ = 0, focusDir_ = 0;
    double panSpeed_ = 0.5, tiltSpeed_ = 0.5, zoomSpeed_ = 0.5;
    bool moveDirty_ = false;
    bool focusDirty_ = false;
    bool stopPending_ = false;
    bool stopTried_ = false;
    TimePoint lastRenew_{};
    TimePoint lastSend_{};
    bool everSent_ = false;
    TimePoint lastStatusPoll_{};
    bool presetsDirty_ = false;
    std::deque<Op> queue_;
    std::vector<PresetInfo> presets_;
    bool presetsValid_ = false;
    CameraStatus status_;
    bool statusValid_ = false;
    GuardStats stats_;
};

}  // namespace hb::ptz
