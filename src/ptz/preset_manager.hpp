#pragma once
// Connects the UI preset buttons (index 0..N-1) to a controller and the PresetStore,
// honoring the per-camera config: preset count, numbering base (0/1) and where the
// position lives (on the camera, or in software via absolute moves).
#include <string>

#include "ptz/controller.hpp"
#include "ptz/preset_store.hpp"

namespace hb::ptz {

class PresetManager {
public:
    PresetManager(PTZController& controller, PresetStore& store, std::string cameraId)
        : ctl_(controller), store_(store), cam_(std::move(cameraId)) {}

    PtzResult recall(int button);
    // Camera storage: asks the camera to store the current position. Software storage: reads
    // the current position (getCameraStatus) and keeps it in the store. Name is always kept
    // in the store.
    PtzResult save(int button, const std::string& name);
    PtzResult clear(int button);
    PtzResult rename(int button, const std::string& name);

    // Label for the button ("" if unused).
    std::string label(int button) const;
    bool isUsed(int button) const;

private:
    PTZController& ctl_;
    PresetStore& store_;
    std::string cam_;
};

}  // namespace hb::ptz
