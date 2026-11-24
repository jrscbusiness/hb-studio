#pragma once
// Cost estimate of an ISO recording plan (disk rate, encoder load) and the "safe cap" per hardware tier.
// Pure arithmetic, no hardware access: the numbers are HEURISTICS to be calibrated on real machines
// (see docs/GRAVACAO_MULTIPISTA.md, "Limites e honestidade").
#include <cstdint>
#include <string>
#include <vector>

#include "core/hardware_profile.hpp"

namespace hb::multitrack {

struct VideoPlan {
    int width = 1920, height = 1080;
    double fps = 30.0;
    int bitrateKbps = 0;            // 0 = automatic (autoVideoBitrateKbps)
};

struct AudioPlan {
    int sampleRate = 48000;
    int channels = 2;
    int bitsPerSample = 24;         // PCM 16/24/32; ignored when compressedKbps > 0
    int compressedKbps = 0;         // AAC/FLAC-ish estimate; 0 = uncompressed PCM
};

struct EstimateInput {
    std::vector<VideoPlan> video;   // ISO cameras
    std::vector<AudioPlan> audio;   // ISO audio channels
    core::Tier tier = core::Tier::Medium;
    bool hwEncode = false;          // a hardware H.264 encoder is believed to be present
    int baseEncodes = 1;            // encodes already running: the Program recording (add the stream's if any)
    double programKbps = 8000;      // bitrate of the Program recording (counted in the disk rate)
    std::uint64_t freeBytes = 0;    // 0 = unknown
    std::uint64_t minFreeBytes = 0; // the ISO tracks stop below this
    bool expertOverride = false;
};

struct Estimate {
    int videoTracks = 0, audioTracks = 0;
    double videoMBps = 0, audioMBps = 0, programMBps = 0, totalMBps = 0;  // MB (10^6 bytes) per second
    double gbPerHour = 0;                  // 10^9 bytes
    double encodeUnits = 0;                // ISO video only, in "1080p30 software encodes"
    double baseUnits = 0;                  // Program/stream encodes already running
    double budgetUnits = 0;                // safe cap of this tier
    double loadPercent = 0;                // (ISO + base) / budget * 100
    int maxVideoTracksSafe = 0;            // how many more cameras of the same size fit in the budget
    core::BudgetVerdict cpuVerdict = core::BudgetVerdict::Ok;
    core::BudgetVerdict diskVerdict = core::BudgetVerdict::Ok;
    core::BudgetVerdict verdict = core::BudgetVerdict::Ok;   // the worse of the two
    double minutesOfDisk = -1;             // -1 = unknown
    std::vector<std::string> notes;        // pt-BR, shown to the user
};

// Bits per pixel per frame for the automatic ISO bitrate (generous: it is meant for editing).
int autoVideoBitrateKbps(int width, int height, double fps);

// Heuristic budget in 1080p30 software-encode equivalents, Program encode included.
double budgetUnitsFor(core::Tier tier);
// Relative cost of one H.264 encode (1.0 = 1080p30 on a software encoder).
double encodeUnitsFor(const VideoPlan& v, bool hwEncode);

Estimate estimate(const EstimateInput& in);

// Largest ISO camera count of the given format that stays within the safe budget (>= 0).
int maxSafeVideoTracks(core::Tier tier, bool hwEncode, const VideoPlan& each, int baseEncodes);

}  // namespace hb::multitrack
