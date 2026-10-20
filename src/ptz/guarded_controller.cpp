#include "ptz/guarded_controller.hpp"

#include <algorithm>
#include <exception>

namespace hb::ptz {

namespace {
template <class F>
PtzResult safely(F&& f) {
    try {
        return f();
    } catch (const std::exception& e) {
        return PtzResult::error(std::string("exception: ") + e.what());
    } catch (...) {
        return PtzResult::error("unknown exception");
    }
}
}  // namespace

GuardedController::GuardedController(std::unique_ptr<PTZProvider> provider, std::shared_ptr<IPtzClock> clock,
                                     GuardConfig cfg, ErrorCallback onError)
    : provider_(std::move(provider)), clock_(std::move(clock)), cfg_(cfg), onError_(std::move(onError)) {
    cfg_.maxRateHz = std::clamp(cfg_.maxRateHz, 1.0, 20.0);  // hard cap: 20 Hz
    minInterval_ = Millis(static_cast<long long>(1000.0 / cfg_.maxRateHz + 0.999));
    if (cfg_.watchdog.count() < 50) cfg_.watchdog = Millis(50);
}

void GuardedController::report(const std::string& op, const PtzResult& r) {
    if (r.ok()) return;
    {
        std::lock_guard lk(mu_);
        ++stats_.failed;
    }
    if (onError_) {
        try {
            onError_(op, r);
        } catch (...) {
        }
    }
}

GuardStats GuardedController::stats() const {
    std::lock_guard lk(mu_);
    return stats_;
}

MoveVector GuardedController::vector() const {
    return {panDir_ * panSpeed_, tiltDir_ * tiltSpeed_, zoomDir_ * zoomSpeed_};
}

PtzResult GuardedController::connect() {
    auto r = safely([&] { return provider_->connect(); });
    const auto now = clock_->now();
    {
        std::lock_guard lk(mu_);
        if (r.ok()) {
            connected_ = true;
            panDir_ = tiltDir_ = zoomDir_ = focusDir_ = 0;
            moveDirty_ = focusDirty_ = stopPending_ = false;
            queue_.clear();
            presetsDirty_ = provider_->capabilities().presetList;
            presetsValid_ = statusValid_ = false;
            lastStatusPoll_ = now - cfg_.statusPoll;
            everSent_ = false;
        }
    }
    report("connect", r);
    return r;
}

PtzResult GuardedController::disconnect() {
    {
        std::lock_guard lk(mu_);
        if (!connected_) return PtzResult::success();
        connected_ = false;
        panDir_ = tiltDir_ = zoomDir_ = focusDir_ = 0;
        moveDirty_ = focusDirty_ = stopPending_ = false;
        queue_.clear();
    }
    // Stop is mandatory before the link goes down, regardless of the rate limiter.
    auto s = safely([&] { return provider_->stop(); });
    auto r = safely([&] { return provider_->disconnect(); });
    report("disconnect/stop", s);
    report("disconnect", r);
    return s.ok() ? r : s;
}

PtzResult GuardedController::setDir(Axis a, int dir) {
    const auto& c = capabilities();
    if (a == Axis::Zoom ? !c.zoom : !c.panTilt) return PtzResult::notSupported(a == Axis::Zoom ? "zoom" : "pan/tilt");
    const auto now = clock_->now();
    std::lock_guard lk(mu_);
    if (!connected_) return PtzResult::error("not connected");
    int& d = a == Axis::Pan ? panDir_ : (a == Axis::Tilt ? tiltDir_ : zoomDir_);
    if (d != dir) moveDirty_ = true;
    d = dir;
    lastRenew_ = now;  // renews the watchdog even if nothing changed
    return PtzResult::success();
}

PtzResult GuardedController::stop() {
    std::lock_guard lk(mu_);
    if (!connected_) return PtzResult::error("not connected");
    panDir_ = tiltDir_ = zoomDir_ = focusDir_ = 0;
    moveDirty_ = focusDirty_ = false;
    stopPending_ = true;
    return PtzResult::success();
}

PtzResult GuardedController::setPanSpeed(double s) {
    std::lock_guard lk(mu_);
    panSpeed_ = PTZProvider::clamp01(s);
    if (panDir_) moveDirty_ = true;
    return PtzResult::success();
}
PtzResult GuardedController::setTiltSpeed(double s) {
    std::lock_guard lk(mu_);
    tiltSpeed_ = PTZProvider::clamp01(s);
    if (tiltDir_) moveDirty_ = true;
    return PtzResult::success();
}
PtzResult GuardedController::setZoomSpeed(double s) {
    std::lock_guard lk(mu_);
    zoomSpeed_ = PTZProvider::clamp01(s);
    if (zoomDir_) moveDirty_ = true;
    return PtzResult::success();
}

PtzResult GuardedController::enqueue(const char* name, bool supported, std::function<PtzResult(PTZProvider&)> run) {
    if (!supported) return PtzResult::notSupported(name);
    std::lock_guard lk(mu_);
    if (!connected_) return PtzResult::error("not connected");
    if (queue_.size() >= cfg_.maxQueue) {
        ++stats_.droppedQueueFull;
        return PtzResult::error("command queue full");
    }
    queue_.push_back({name, std::move(run)});
    return PtzResult::success();
}

PtzResult GuardedController::gotoPreset(int id) {
    return enqueue("gotoPreset", capabilities().presetRecall, [id](PTZProvider& p) { return p.gotoPreset(id); });
}
PtzResult GuardedController::savePreset(int id) {
    auto r = enqueue("savePreset", capabilities().presetSave, [id](PTZProvider& p) { return p.savePreset(id); });
    if (r.ok()) {
        std::lock_guard lk(mu_);
        presetsDirty_ = capabilities().presetList;
    }
    return r;
}
PtzResult GuardedController::deletePreset(int id) {
    auto r = enqueue("deletePreset", capabilities().presetDelete, [id](PTZProvider& p) { return p.deletePreset(id); });
    if (r.ok()) {
        std::lock_guard lk(mu_);
        presetsDirty_ = capabilities().presetList;
    }
    return r;
}

PtzReply<std::vector<PresetInfo>> GuardedController::getPresets() {
    if (!capabilities().presetList) return {PtzResult::notSupported("preset listing"), {}};
    std::lock_guard lk(mu_);
    if (!presetsValid_) return {PtzResult::error("presets not loaded yet"), {}};
    return {PtzResult::success(), presets_};
}

PtzResult GuardedController::focusAuto() {
    return enqueue("focusAuto", capabilities().focusAuto, [](PTZProvider& p) { return p.focusAuto(); });
}
PtzResult GuardedController::focusManual() {
    return enqueue("focusManual", capabilities().focusManual, [](PTZProvider& p) { return p.focusManual(); });
}
PtzResult GuardedController::irisOpen() {
    return enqueue("irisOpen", capabilities().iris, [](PTZProvider& p) { return p.irisOpen(); });
}
PtzResult GuardedController::irisClose() {
    return enqueue("irisClose", capabilities().iris, [](PTZProvider& p) { return p.irisClose(); });
}

PtzResult GuardedController::focusNear() {
    if (!capabilities().focusDrive) return PtzResult::notSupported("focus drive");
    const auto now = clock_->now();
    std::lock_guard lk(mu_);
    if (!connected_) return PtzResult::error("not connected");
    if (focusDir_ != -1) focusDirty_ = true;
    focusDir_ = -1;
    lastRenew_ = now;
    return PtzResult::success();
}
PtzResult GuardedController::focusFar() {
    if (!capabilities().focusDrive) return PtzResult::notSupported("focus drive");
    const auto now = clock_->now();
    std::lock_guard lk(mu_);
    if (!connected_) return PtzResult::error("not connected");
    if (focusDir_ != 1) focusDirty_ = true;
    focusDir_ = 1;
    lastRenew_ = now;
    return PtzResult::success();
}

PtzResult GuardedController::moveAbsolute(const PtzPosition& p) {
    return enqueue("moveAbsolute", capabilities().absoluteMove, [p](PTZProvider& pr) { return pr.moveAbsolute(p); });
}

PtzReply<CameraStatus> GuardedController::getCameraStatus() {
    std::lock_guard lk(mu_);
    CameraStatus s = statusValid_ ? status_ : CameraStatus{};
    s.connected = connected_;
    s.provider = provider_->name();
    s.moving = panDir_ || tiltDir_ || zoomDir_ || focusDir_;
    if (!capabilities().statusInquiry) return {PtzResult::notSupported("status inquiry"), s};
    if (!statusValid_) return {PtzResult::error("no status yet"), s};
    return {PtzResult::success(), s};
}

void GuardedController::tick() {
    const auto now = clock_->now();
    enum class Job { None, Stop, Move, Focus, Queued, Status, Presets } job = Job::None;
    MoveVector vec;
    int focusDir = 0;
    Op op;
    bool watchdogFired = false;
    {
        std::lock_guard lk(mu_);
        if (!connected_) return;
        const bool moving = panDir_ || tiltDir_ || zoomDir_ || focusDir_;
        if (moving && now - lastRenew_ >= cfg_.watchdog) {
            panDir_ = tiltDir_ = zoomDir_ = focusDir_ = 0;
            moveDirty_ = focusDirty_ = false;
            stopPending_ = true;
            ++stats_.watchdogStops;
            watchdogFired = true;
        }
        const bool slotFree = !everSent_ || now - lastSend_ >= minInterval_;
        if (stopPending_ && (slotFree || !stopTried_)) {
            // First Stop attempt bypasses the limiter; retries obey it.
            job = Job::Stop;
        } else if (slotFree) {
            if (moveDirty_) {
                job = Job::Move;
                vec = vector();
                moveDirty_ = false;
            } else if (focusDirty_) {
                job = Job::Focus;
                focusDir = focusDir_;
                focusDirty_ = false;
            } else if (!queue_.empty()) {
                job = Job::Queued;
                op = std::move(queue_.front());
                queue_.pop_front();
            } else if (presetsDirty_) {
                job = Job::Presets;
                presetsDirty_ = false;
            } else if (cfg_.statusPoll.count() > 0 && provider_->capabilities().statusInquiry &&
                       now - lastStatusPoll_ >= cfg_.statusPoll) {
                job = Job::Status;
                lastStatusPoll_ = now;
            }
        }
        if (job != Job::None) {
            everSent_ = true;
            lastSend_ = now;
            ++stats_.sent;
        }
        if (job == Job::Stop) stopTried_ = true;
    }
    if (watchdogFired) report("watchdog", PtzResult::error("movement not renewed: Stop sent"));

    switch (job) {
        case Job::None: break;
        case Job::Stop: {
            auto r = safely([&] { return provider_->stop(); });
            {
                std::lock_guard lk(mu_);
                if (r.ok()) {
                    stopPending_ = false;
                    stopTried_ = false;
                }
            }
            report("stop", r);
            break;
        }
        case Job::Move: report("drive", safely([&] { return provider_->drive(vec); })); break;
        case Job::Focus:
            report("focus", safely([&] { return focusDir < 0 ? provider_->focusNear() : provider_->focusFar(); }));
            break;
        case Job::Queued: report(op.name, safely([&] { return op.run(*provider_); })); break;
        case Job::Presets: {
            auto r = safely([&] {
                auto p = provider_->getPresets();
                if (p.ok()) {
                    std::lock_guard lk(mu_);
                    presets_ = std::move(p.value);
                    presetsValid_ = true;
                }
                return p.result;
            });
            report("getPresets", r);
            break;
        }
        case Job::Status: {
            auto r = safely([&] {
                auto s = provider_->getCameraStatus();
                if (s.ok()) {
                    std::lock_guard lk(mu_);
                    status_ = std::move(s.value);
                    statusValid_ = true;
                }
                return s.result;
            });
            report("status", r);
            break;
        }
    }
}

}  // namespace hb::ptz
