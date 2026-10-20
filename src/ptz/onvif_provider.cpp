#include "ptz/onvif_provider.hpp"

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
double clampUnit(double v) { return v != v ? 0 : std::clamp(v, -1.0, 1.0); }
}  // namespace

OnvifPtzProvider::OnvifPtzProvider(std::shared_ptr<IOnvifPtzBackend> backend) : b_(std::move(backend)) {
    refreshCaps();
}

void OnvifPtzProvider::refreshCaps() {
    caps_ = {};
    if (!b_) return;
    bcaps_ = b_->capabilities();
    caps_.panTilt = caps_.zoom = caps_.variableSpeed = bcaps_.ptz;
    caps_.presetRecall = caps_.presetSave = caps_.presetDelete = caps_.presetList = bcaps_.presets;
    caps_.focusDrive = caps_.focusManual = bcaps_.imagingFocus;
    caps_.focusAuto = bcaps_.autoFocus;
    caps_.iris = bcaps_.iris;
    caps_.statusInquiry = bcaps_.status;
    caps_.absoluteMove = bcaps_.absoluteMove;
    caps_.maxPresetId = bcaps_.presets ? bcaps_.maxPresets : 0;
}

PtzResult OnvifPtzProvider::connect() {
    return safely([&] {
        std::lock_guard lk(mu_);
        if (!b_) return PtzResult::error("no backend");
        auto r = b_->connect();
        if (r.ok()) {
            connected_ = true;
            refreshCaps();  // the backend may learn its capabilities during connect
        }
        return r;
    });
}

PtzResult OnvifPtzProvider::disconnect() {
    PtzResult stopped = PtzResult::success();
    if (connected_) stopped = stop();
    return safely([&] {
        std::lock_guard lk(mu_);
        auto r = b_ ? b_->disconnect() : PtzResult::success();
        connected_ = false;
        return stopped.ok() ? r : stopped;
    });
}

PtzResult OnvifPtzProvider::drive(const MoveVector& v) {
    if (!caps_.panTilt) return PtzResult::notSupported("ONVIF PTZ service");
    return safely([&] {
        std::lock_guard lk(mu_);
        if (!connected_) return PtzResult::error("not connected");
        if (v.zero()) return b_->stop(true, true);
        return b_->continuousMove(clampUnit(v.pan), clampUnit(v.tilt), clampUnit(v.zoom));
    });
}

PtzResult OnvifPtzProvider::stopAll() {
    if (!caps_.panTilt) return PtzResult::notSupported("ONVIF PTZ service");
    return safely([&] {
        std::lock_guard lk(mu_);
        if (!connected_) return PtzResult::error("not connected");
        return b_->stop(true, true);
    });
}

PtzResult OnvifPtzProvider::stopLens() {
    return safely([&] {
        std::lock_guard lk(mu_);
        if (!lensActive_) return PtzResult::success();
        auto r = b_->focusStop();
        if (r.ok()) lensActive_ = false;
        return r;
    });
}

PtzResult OnvifPtzProvider::moveAbsolute(const PtzPosition& p) {
    if (!bcaps_.absoluteMove) return PtzResult::notSupported("absolute move");
    if (p.units != PositionUnits::Normalized) return PtzResult::error("ONVIF needs normalized units");
    return safely([&] { std::lock_guard lk(mu_); return b_->absoluteMove(p); });
}

#define HB_REQUIRE(cap, what) \
    if (!(cap)) return PtzResult::notSupported(what)

PtzResult OnvifPtzProvider::gotoPreset(int id) {
    HB_REQUIRE(caps_.presetRecall, "presets");
    return safely([&] { std::lock_guard lk(mu_); return b_->gotoPreset(id); });
}
PtzResult OnvifPtzProvider::savePreset(int id) {
    HB_REQUIRE(caps_.presetSave, "presets");
    return safely([&] { std::lock_guard lk(mu_); return b_->setPreset(id, std::string{}); });
}
PtzResult OnvifPtzProvider::deletePreset(int id) {
    HB_REQUIRE(caps_.presetDelete, "presets");
    return safely([&] { std::lock_guard lk(mu_); return b_->removePreset(id); });
}
PtzReply<std::vector<PresetInfo>> OnvifPtzProvider::getPresets() {
    if (!caps_.presetList) return {PtzResult::notSupported("presets"), {}};
    PtzReply<std::vector<PresetInfo>> out;
    out.result = safely([&] {
        std::lock_guard lk(mu_);
        auto r = b_->getPresets();
        out.value = std::move(r.value);
        return r.result;
    });
    return out;
}

PtzResult OnvifPtzProvider::focusAuto() {
    HB_REQUIRE(caps_.focusAuto, "auto focus");
    return safely([&] { std::lock_guard lk(mu_); return b_->setFocusAuto(true); });
}
PtzResult OnvifPtzProvider::focusManual() {
    HB_REQUIRE(caps_.focusManual, "manual focus");
    return safely([&] { std::lock_guard lk(mu_); return b_->setFocusAuto(false); });
}
PtzResult OnvifPtzProvider::focusNear() {
    HB_REQUIRE(caps_.focusDrive, "focus drive");
    return safely([&] {
        std::lock_guard lk(mu_);
        auto r = b_->focusMove(-0.5);
        if (r.ok()) lensActive_ = true;
        return r;
    });
}
PtzResult OnvifPtzProvider::focusFar() {
    HB_REQUIRE(caps_.focusDrive, "focus drive");
    return safely([&] {
        std::lock_guard lk(mu_);
        auto r = b_->focusMove(0.5);
        if (r.ok()) lensActive_ = true;
        return r;
    });
}
PtzResult OnvifPtzProvider::irisOpen() {
    HB_REQUIRE(caps_.iris, "iris");
    return safely([&] { std::lock_guard lk(mu_); return b_->irisStep(1); });
}
PtzResult OnvifPtzProvider::irisClose() {
    HB_REQUIRE(caps_.iris, "iris");
    return safely([&] { std::lock_guard lk(mu_); return b_->irisStep(-1); });
}

PtzReply<CameraStatus> OnvifPtzProvider::getCameraStatus() {
    if (!caps_.statusInquiry) return {PtzResult::notSupported("status"), {}};
    PtzReply<CameraStatus> out;
    out.result = safely([&] {
        std::lock_guard lk(mu_);
        out.value.provider = name();
        out.value.connected = connected_;
        auto r = b_->getPosition();
        if (r.ok()) {
            r.value.units = PositionUnits::Normalized;
            out.value.position = r.value;
        }
        return r.result;
    });
    return out;
}

#undef HB_REQUIRE

}  // namespace hb::ptz
