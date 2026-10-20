#include "ptz/pelco_provider.hpp"

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

PelcoProvider::PelcoProvider(std::unique_ptr<ITransport> transport, PelcoConfig cfg)
    : t_(std::move(transport)), cfg_(cfg) {
    caps_.panTilt = caps_.zoom = caps_.variableSpeed = true;
    caps_.presetRecall = caps_.presetSave = caps_.presetDelete = true;
    caps_.focusManual = caps_.focusDrive = caps_.iris = true;  // no auto-focus, no inquiries
    caps_.maxPresetId = cfg_.maxPresetId;
}

PtzResult PelcoProvider::sendCmd(const pelco::Command& c) {
    if (!t_ || !t_->isOpen()) return PtzResult::error("not connected");
    const Bytes f = cfg_.variant == PelcoVariant::D ? pelco::frameD(cfg_.address, c) : pelco::frameP(cfg_.address, c);
    auto r = t_->send(f);
    return r.ok ? PtzResult::success() : PtzResult::error("send: " + r.error);
}

PtzResult PelcoProvider::connect() {
    return safely([&] {
        std::lock_guard lk(mu_);
        if (!t_) return PtzResult::error("no transport");
        if (auto r = t_->open(); !r.ok) return PtzResult::error("open: " + r.error);
        connected_ = true;
        return PtzResult::success();
    });
}

PtzResult PelcoProvider::disconnect() {
    PtzResult stopped = PtzResult::success();
    if (connected_) stopped = stop();
    return safely([&] {
        std::lock_guard lk(mu_);
        if (t_) t_->close();
        connected_ = false;
        return stopped;
    });
}

PtzResult PelcoProvider::drive(const MoveVector& v) {
    return safely([&] {
        std::lock_guard lk(mu_);
        return sendCmd(v.zero() ? pelco::stopCommand() : pelco::motion(v.pan, v.tilt, v.zoom));
    });
}

PtzResult PelcoProvider::stopLens() {
    return safely([&] {
        std::lock_guard lk(mu_);
        if (!lensActive_) return PtzResult::success();
        auto r = sendCmd(pelco::stopCommand());
        if (r.ok()) lensActive_ = false;
        return r;
    });
}

PtzResult PelcoProvider::checkPreset(int id) const {
    if (id < 0 || id > cfg_.maxPresetId) return PtzResult::error("preset id out of range");
    return PtzResult::success();
}

PtzResult PelcoProvider::gotoPreset(int id) {
    if (auto c = checkPreset(id); !c.ok()) return c;
    return safely([&] { std::lock_guard lk(mu_); return sendCmd(pelco::presetCall(id)); });
}
PtzResult PelcoProvider::savePreset(int id) {
    if (auto c = checkPreset(id); !c.ok()) return c;
    return safely([&] { std::lock_guard lk(mu_); return sendCmd(pelco::presetSet(id)); });
}
PtzResult PelcoProvider::deletePreset(int id) {
    if (auto c = checkPreset(id); !c.ok()) return c;
    return safely([&] { std::lock_guard lk(mu_); return sendCmd(pelco::presetClear(id)); });
}
PtzReply<std::vector<PresetInfo>> PelcoProvider::getPresets() {
    return {PtzResult::notSupported("Pelco cannot list presets"), {}};
}

PtzResult PelcoProvider::focusAuto() { return PtzResult::notSupported("Pelco has no auto-focus command"); }
PtzResult PelcoProvider::focusManual() { return PtzResult::notSupported("Pelco has no focus mode command"); }
PtzResult PelcoProvider::focusNear() {
    return safely([&] {
        std::lock_guard lk(mu_);
        auto r = sendCmd(pelco::focus(-1));
        if (r.ok()) lensActive_ = true;
        return r;
    });
}
PtzResult PelcoProvider::focusFar() {
    return safely([&] {
        std::lock_guard lk(mu_);
        auto r = sendCmd(pelco::focus(1));
        if (r.ok()) lensActive_ = true;
        return r;
    });
}
// Iris is pulsed: command followed by an immediate stop frame.
PtzResult PelcoProvider::irisOpen() {
    return safely([&] {
        std::lock_guard lk(mu_);
        auto r = sendCmd(pelco::iris(1));
        auto s = sendCmd(pelco::stopCommand());
        return r.ok() ? s : r;
    });
}
PtzResult PelcoProvider::irisClose() {
    return safely([&] {
        std::lock_guard lk(mu_);
        auto r = sendCmd(pelco::iris(-1));
        auto s = sendCmd(pelco::stopCommand());
        return r.ok() ? s : r;
    });
}
PtzReply<CameraStatus> PelcoProvider::getCameraStatus() {
    return {PtzResult::notSupported("Pelco has no inquiries"), {}};
}

}  // namespace hb::ptz
