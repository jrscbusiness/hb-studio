#include "ptz/preset_manager.hpp"

namespace hb::ptz {

PtzResult PresetManager::recall(int button) {
    const auto cfg = store_.config(cam_);
    const int id = cfg.deviceId(button);
    if (id < 0) return PtzResult::error("preset button out of range");
    if (cfg.storage == PresetStorage::Camera) return ctl_.gotoPreset(id);
    auto e = store_.find(cam_, id);
    if (!e || !e->position) return PtzResult::error("preset is empty");
    return ctl_.moveAbsolute(*e->position);
}

PtzResult PresetManager::save(int button, const std::string& name) {
    const auto cfg = store_.config(cam_);
    const int id = cfg.deviceId(button);
    if (id < 0) return PtzResult::error("preset button out of range");
    PresetEntry e;
    e.id = id;
    e.name = name;
    if (cfg.storage == PresetStorage::Camera) {
        auto r = ctl_.savePreset(id);
        if (!r.ok()) return r;
    } else {
        auto s = ctl_.getCameraStatus();
        if (!s.ok() || !s.value.position) {
            return s.ok() ? PtzResult::error("camera reported no position") : s.result;
        }
        e.position = s.value.position;
    }
    store_.set(cam_, std::move(e));
    return PtzResult::success();
}

PtzResult PresetManager::clear(int button) {
    const auto cfg = store_.config(cam_);
    const int id = cfg.deviceId(button);
    if (id < 0) return PtzResult::error("preset button out of range");
    if (cfg.storage == PresetStorage::Camera) {
        auto r = ctl_.deletePreset(id);
        if (!r.ok()) return r;
    }
    store_.remove(cam_, id);
    return PtzResult::success();
}

PtzResult PresetManager::rename(int button, const std::string& name) {
    const int id = store_.config(cam_).deviceId(button);
    if (id < 0) return PtzResult::error("preset button out of range");
    auto e = store_.find(cam_, id);
    if (!e) return PtzResult::error("preset is empty");
    e->name = name;
    store_.set(cam_, *e);
    return PtzResult::success();
}

std::string PresetManager::label(int button) const {
    const int id = store_.config(cam_).deviceId(button);
    auto e = id < 0 ? std::nullopt : store_.find(cam_, id);
    return e ? e->name : std::string{};
}

bool PresetManager::isUsed(int button) const {
    const int id = store_.config(cam_).deviceId(button);
    return id >= 0 && store_.find(cam_, id).has_value();
}

}  // namespace hb::ptz
