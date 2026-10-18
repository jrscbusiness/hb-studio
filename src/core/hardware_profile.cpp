#include "core/hardware_profile.hpp"

#include <fstream>
#include <thread>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

namespace hb::core {

const char* tierName(Tier t) {
    switch (t) {
        case Tier::Low: return "LOW";
        case Tier::Medium: return "MEDIUM";
        case Tier::High: return "HIGH";
        case Tier::Pro: return "PRO";
    }
    return "?";
}

const char* latencyModeName(LatencyMode m) {
    switch (m) {
        case LatencyMode::Low: return "LOW LATENCY";
        case LatencyMode::Balanced: return "BALANCED";
        case LatencyMode::Stable: return "STABLE";
    }
    return "?";
}

HardwareInfo detectHardware() {
    HardwareInfo hw;
    hw.logicalCores = std::thread::hardware_concurrency();
#ifdef _WIN32
    MEMORYSTATUSEX m{};
    m.dwLength = sizeof(m);
    if (GlobalMemoryStatusEx(&m)) hw.ramBytes = m.ullTotalPhys;
#elif defined(__linux__)
    std::ifstream f("/proc/meminfo");
    std::string key;
    std::uint64_t kb = 0;
    while (f >> key >> kb) {
        if (key == "MemTotal:") {
            hw.ramBytes = kb * 1024;
            break;
        }
        f.ignore(256, '\n');
    }
#endif
    return hw;
}

namespace {
constexpr std::uint64_t GiB = 1024ull * 1024 * 1024;
}

Tier classify(const HardwareInfo& hw) {
    const bool hwDecode = hw.hwDecodeH264;
    Tier t = Tier::Low;
    if (hw.logicalCores >= 4 && hw.ramBytes >= 8 * GiB) t = Tier::Medium;
    if (hwDecode && hw.logicalCores >= 8 && hw.ramBytes >= 16 * GiB) t = Tier::High;
    if (hwDecode && hw.logicalCores >= 16 && hw.ramBytes >= 32 * GiB && hw.vramBytes >= 8 * GiB)
        t = Tier::Pro;
    return t;
}

const TierPreset& presetFor(Tier tier) {
    // Initial values; to be calibrated with real benchmarks (Phase 16).
    static const TierPreset presets[] = {
        {Tier::Low, 4, 6, {480, 270}, 10},
        {Tier::Medium, 8, 10, {640, 360}, 15},
        {Tier::High, 12, 16, {640, 360}, 15},
        {Tier::Pro, 16, 24, {960, 540}, 20},
    };
    return presets[static_cast<int>(tier)];
}

const std::vector<OutputPreset>& commonOutputPresets() {
    static const std::vector<OutputPreset> p = {
        {"720p30", {1280, 720}, {30, 1}},
        {"720p60", {1280, 720}, {60, 1}},
        {"1080p25", {1920, 1080}, {25, 1}},
        {"1080p30", {1920, 1080}, {30, 1}},
        {"1080p60", {1920, 1080}, {60, 1}},
        {"2160p30 (4K)", {3840, 2160}, {30, 1}},
    };
    return p;
}

LatencyParams latencyParams(LatencyMode mode) {
    switch (mode) {
        case LatencyMode::Low: return {50, 2, 500, 5000, true};
        case LatencyMode::Balanced: return {200, 4, 1000, 10000, true};
        case LatencyMode::Stable: return {1000, 12, 2000, 15000, false};
    }
    return {200, 4, 1000, 10000, true};
}

BudgetResult checkSourceBudget(Tier tier, int sources, bool expertOverride) {
    const auto& p = presetFor(tier);
    if (sources > p.hardMaxSources) {
        if (expertOverride)
            return {BudgetVerdict::Warning,
                    "Source count exceeds the hard limit for this computer; expert override is on."};
        return {BudgetVerdict::Blocked,
                "PERFORMANCE: this computer cannot handle that many sources. "
                "Reduce sources or enable expert override in Settings."};
    }
    if (sources > p.recommendedSources)
        return {BudgetVerdict::Warning,
                "PERFORMANCE WARNING: source count is above the recommended capacity of this "
                "computer. Suggestion: reduce Preview Quality."};
    return {BudgetVerdict::Ok, ""};
}

}  // namespace hb::core
