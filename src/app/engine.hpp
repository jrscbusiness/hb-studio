#pragma once
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

// Application layer: owns the event bus, logger, test sources, switcher and
// Program Output, independent of any window. The UI only calls this class.
class Engine {
public:
    static constexpr int kSourceCount = 8;
    static constexpr int kMonitorW = 640, kMonitorH = 360;  // PVW/PGM monitors in the UI
    static constexpr int kTileW = 192, kTileH = 108;        // input tiles

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

    // UI frames: "pgm", "pvw", "s0".."s7". Returns false for unknown ids.
    bool renderUiFrame(const std::string& id, video::Frame& out);

private:
    core::EventBus bus_;
    core::Logger logger_;
    std::shared_ptr<core::MemorySink> log_;
    core::HardwareInfo hw_;
    core::Tier tier_;
    std::vector<std::shared_ptr<const video::Source>> sources_;
    std::unique_ptr<switcher::Switcher> switcher_;
    std::unique_ptr<video::ProgramOutput> program_;

    std::mutex mutex_;
    std::uint64_t lastProduced_ = 0;
    std::chrono::steady_clock::time_point lastSample_{};
    double fps_ = 0;
    video::Frame pgmFull_;
};

}  // namespace hb::app
