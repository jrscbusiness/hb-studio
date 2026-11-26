#pragma once
// The 15 Hz "s" state message sent to the UI, built from a plain snapshot so the JSON layout is
// testable without Windows, the engine or any device. app_main.cpp fills the snapshot from the live
// objects (see collectState there) and calls buildStateJson().
#include <cstdint>
#include <string>
#include <vector>

namespace hb::app {

struct StateSlot {
    std::string kind = "test";    // "test" | "dev"
    std::string status = "test";  // test | online | connecting | nosignal | error | off
    bool enabled = true;
    int width = 0, height = 0;
    double fps = 0;
    std::string label, deviceId, message;
};

struct StateLogLine {
    std::string text;   // already formatted ("HH:MM:SS [level] [module] message")
    std::string level;  // level name, plain ASCII
};

struct StateSnapshot {
    std::string version;  // HB_VERSION
    int pvw = 0, pgm = 0;
    bool transitioning = false;
    double progress = 0;
    double fps = 0;
    int targetFps = 30;
    int programWidth = 0, programHeight = 0;
    std::uint64_t frames = 0, skipped = 0;
    std::string tier;
    unsigned cores = 0, ramGiB = 0;
    int sourcesRecommended = 0, sourcesMax = 0;
    struct VCam {
        bool supported = false, installed = false, active = false, connected = false, busy = false, error = false;
        std::uint64_t sent = 0;
        std::string message;
    } vcam;
    int toastId = 0;  // <= 0: no toast
    std::string toast;
    std::vector<StateSlot> slots;
    std::vector<StateLogLine> log;
};

std::string buildStateJson(const StateSnapshot& s);

}  // namespace hb::app
