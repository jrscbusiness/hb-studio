// hb_probe: headless smoke test of the Phase 1 core.
// Prints detected hardware + suggested tier, then runs the Program Output
// (test pattern, no window) for a few seconds and reports real timing.
//
//   hb_probe [seconds=3] [preset=1080p30]
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

#include "core/hardware_profile.hpp"
#include "core/logger.hpp"
#include "video/program_output.hpp"

using namespace hb;

int main(int argc, char** argv) {
    const int seconds = argc > 1 ? std::max(1, std::atoi(argv[1])) : 3;
    const std::string presetName = argc > 2 ? argv[2] : "1080p30";

    core::Logger log;
    log.addSink(std::make_shared<core::StreamSink>(std::cout));

    const auto hw = core::detectHardware();
    const auto tier = core::classify(hw);
    log.info("hardware", "cores=" + std::to_string(hw.logicalCores) +
                             " ramGiB=" + std::to_string(hw.ramBytes >> 30) +
                             " suggestedTier=" + core::tierName(tier));

    video::ProgramFormat fmt;
    for (const auto& p : core::commonOutputPresets()) {
        if (presetName == p.name) {
            fmt = {p.resolution.width, p.resolution.height, p.fps};
            break;
        }
    }
    log.info("program", "format " + std::to_string(fmt.width) + "x" + std::to_string(fmt.height) +
                            "@" + std::to_string(fmt.fps.value()));

    core::EventBus bus;
    bus.subscribe("program.*", [&](const core::Event& e) { log.info("event", e.topic); });

    video::ProgramOutput out(fmt, &bus);
    std::atomic<std::uint64_t> seen{0};
    out.addConsumer([&](const auto&) { seen.fetch_add(1); });
    out.start();
    std::this_thread::sleep_for(std::chrono::seconds(seconds));
    out.stop();

    const auto s = out.clockStats();
    log.info("program", "produced=" + std::to_string(out.framesProduced()) +
                            " consumed=" + std::to_string(seen.load()) +
                            " skipped=" + std::to_string(s.skippedTicks) +
                            " effectiveFps=" + std::to_string(double(out.framesProduced()) / seconds));
    return 0;
}
