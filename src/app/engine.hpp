#pragma once
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/event_bus.hpp"
#include "core/hardware_profile.hpp"
#include "core/logger.hpp"
#include "switcher/switcher.hpp"
#include "video/program_output.hpp"
#include "video/source.hpp"

namespace hb::app {

// One entry of the PVW/PGM monitor and input-tile render sizes used for the UI's own small,
// JPEG-encoded preview images (the "Qualidade do Preview" setting in CONFIG) - entirely separate
// from the Program Output's real resolution (set elsewhere, applies on restart). This exists
// because every one of these UI frames is decoded from its source, composited, and JPEG-encoded
// on every tick regardless of how small the result ends up on screen; a weaker computer with many
// large sources spends real, avoidable CPU on this unless the operator can turn it down.
struct PreviewQualityPreset {
    const char* id;
    const char* label;
    int monitorW, monitorH;   // PVW/PGM monitors
    int tileW, tileH;         // input tiles
};

// Application layer: owns the event bus, logger, test sources, switcher and
// Program Output, independent of any window. The UI only calls this class.
class Engine {
public:
    static constexpr int kSourceCount = 16;  // 2 banks of 8 (see docs/ORCHESTRATION.md / UI bank selector)

    // Index 2 ("alta") matches this app's established defaults (640x360 / 192x108) exactly, so a
    // fresh install or one that never touched this setting behaves exactly as before this existed.
    static const std::vector<PreviewQualityPreset>& previewQualityPresets();
    static constexpr int kDefaultPreviewQualityIndex = 2;

    // Returns the currently selected preset's id (e.g. "alta").
    std::string previewQuality() const;
    // false if `id` doesn't match any preset in previewQualityPresets(); true and applied (takes
    // effect on the very next UI frame - no restart needed, unlike the Program Output resolution).
    bool setPreviewQuality(const std::string& id);

    struct Snapshot {
        switcher::Switcher::State sw;
        double fps = 0;
        std::uint64_t produced = 0, skipped = 0;
        std::string tier;
        unsigned cores = 0;
        unsigned ramGiB = 0;
        int sourcesRecommended = 0, sourcesMax = 0;
        int targetFps = 30;
    };

    explicit Engine(video::ProgramFormat format = {1920, 1080, {30, 1}});
    ~Engine() { stop(); }

    void start();
    void stop();

    switcher::Switcher& switcher() { return *switcher_; }
    video::ProgramOutput& program() { return *program_; }
    core::EventBus& bus() { return bus_; }
    core::Logger& logger() { return logger_; }
    std::vector<core::LogRecord> logTail() const { return log_->snapshot(); }

    // Measured FPS is derived between consecutive calls.
    Snapshot snapshot();

    // Input slots (CAM 1..8): assign a real device (webcam, virtual camera...) or go back to the test source.
    bool assignSlot(int slot, std::shared_ptr<const video::LiveSource> device, std::string deviceId, std::string deviceName);
    bool clearSlot(int slot);
    video::SlotSource::Info slotInfo(int slot) const;
    // ---- FEATURE: chroma ----
    bool setSlotFilter(int slot, std::shared_ptr<video::FrameFilter> filter);
    // ---- /FEATURE: chroma ----

    // UI frames: "pgm", "pvw", "s0".."s7". Returns false for unknown ids.
    bool renderUiFrame(const std::string& id, video::Frame& out);

private:
    core::EventBus bus_;
    core::Logger logger_;
    std::shared_ptr<core::MemorySink> log_;
    core::HardwareInfo hw_;
    core::Tier tier_;
    std::vector<std::shared_ptr<video::SlotSource>> slots_;
    std::vector<std::shared_ptr<const video::Source>> sources_;
    std::unique_ptr<switcher::Switcher> switcher_;
    std::unique_ptr<video::ProgramOutput> program_;

    std::mutex mutex_;
    std::uint64_t lastProduced_ = 0;
    std::chrono::steady_clock::time_point lastSample_{};
    double fps_ = 0;
    video::Frame pgmFull_;

    // Read every tick by renderUiFrame() (the feeder thread) and written occasionally by the UI
    // message handler (a different thread) when the operator changes the setting - atomics avoid
    // taking mutex_ on every single preview frame for something that changes this rarely.
    std::atomic<int> previewQualityIndex_{kDefaultPreviewQualityIndex};
};

}  // namespace hb::app
