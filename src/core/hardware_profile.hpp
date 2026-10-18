#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "core/clock.hpp"

namespace hb::core {

enum class Tier { Low, Medium, High, Pro };
enum class LatencyMode { Low, Balanced, Stable };

const char* tierName(Tier t);
const char* latencyModeName(LatencyMode m);

struct HardwareInfo {
    unsigned logicalCores = 0;
    std::uint64_t ramBytes = 0;
    std::string gpuName;           // filled by the GPU layer (DXGI) later
    std::uint64_t vramBytes = 0;   // 0 = unknown / integrated
    bool hwDecodeH264 = false;
    bool hwDecodeH265 = false;
    bool hwEncode = false;
};

// Portable part only (cores, RAM). GPU/codec fields are filled in by the
// platform GPU layer in a later phase.
HardwareInfo detectHardware();

// Heuristic classification. Without HW decode a machine is capped at MEDIUM.
// Thresholds are initial values to be calibrated with benchmarks.
Tier classify(const HardwareInfo& hw);

struct Resolution {
    int width;
    int height;
};

struct TierPreset {
    Tier tier;
    int recommendedSources;
    int hardMaxSources;
    Resolution preview;  // Multiview tile decode resolution
    int previewFps;
};
const TierPreset& presetFor(Tier tier);

// Most common program output formats, selectable by the user.
struct OutputPreset {
    const char* name;
    Resolution resolution;
    Rational fps;
};
const std::vector<OutputPreset>& commonOutputPresets();

struct LatencyParams {
    int networkBufferMs;
    int decodeQueueFrames;
    int reconnectInitialMs;
    int reconnectMaxMs;
    bool dropOldestOnOverflow;
};
LatencyParams latencyParams(LatencyMode mode);

// User-facing choice: automatic detection or a fixed tier. Everything the
// system suggests can be overridden.
struct HardwareProfileSetting {
    bool automatic = true;
    Tier manual = Tier::Medium;
    Tier effective(const HardwareInfo& hw) const { return automatic ? classify(hw) : manual; }
};

enum class BudgetVerdict { Ok, Warning, Blocked };
struct BudgetResult {
    BudgetVerdict verdict;
    std::string message;
};
// Blocked above the hard limit unless the user explicitly enabled expert
// override (then it degrades to Warning).
BudgetResult checkSourceBudget(Tier tier, int sources, bool expertOverride = false);

}  // namespace hb::core
