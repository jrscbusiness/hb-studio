#include "multitrack/resource_estimator.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace hb::multitrack {

namespace {
constexpr double kRefPixelsPerSec = 1920.0 * 1080.0 * 30.0;
constexpr double kHwWeight = 0.35;   // a hardware encoder costs about a third of the CPU of a software one

std::string fmt1(double v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.1f", v);
    return b;
}
core::BudgetVerdict worse(core::BudgetVerdict a, core::BudgetVerdict b) {
    return static_cast<int>(a) >= static_cast<int>(b) ? a : b;
}
}  // namespace

int autoVideoBitrateKbps(int width, int height, double fps) {
    if (width <= 0 || height <= 0 || fps <= 0) return 8000;
    const double kbps = static_cast<double>(width) * height * fps * 0.13 / 1000.0;   // 1080p30 ~ 8.1 Mbps
    return static_cast<int>(std::clamp(std::lround(kbps / 250.0) * 250L, 1000L, 50000L));
}

double budgetUnitsFor(core::Tier tier) {
    switch (tier) {
        case core::Tier::Low: return 1.5;
        case core::Tier::Medium: return 3.0;
        case core::Tier::High: return 6.0;
        case core::Tier::Pro: return 10.0;
    }
    return 1.5;
}

double encodeUnitsFor(const VideoPlan& v, bool hwEncode) {
    const double px = static_cast<double>(std::max(0, v.width)) * std::max(0, v.height) * std::max(0.0, v.fps);
    return px / kRefPixelsPerSec * (hwEncode ? kHwWeight : 1.0);
}

int maxSafeVideoTracks(core::Tier tier, bool hwEncode, const VideoPlan& each, int baseEncodes) {
    const double unit = encodeUnitsFor(each, hwEncode);
    if (unit <= 0) return 0;
    VideoPlan program;   // the Program encode (1080p30 reference) counts as one base encode
    const double base = std::max(0, baseEncodes) * encodeUnitsFor(program, hwEncode);
    const double room = budgetUnitsFor(tier) - base;
    return room <= 0 ? 0 : static_cast<int>(std::floor(room / unit + 1e-9));
}

Estimate estimate(const EstimateInput& in) {
    Estimate e;
    e.videoTracks = static_cast<int>(in.video.size());
    e.audioTracks = static_cast<int>(in.audio.size());

    double vBytes = 0;
    for (const auto& v : in.video) {
        const int kbps = v.bitrateKbps > 0 ? v.bitrateKbps : autoVideoBitrateKbps(v.width, v.height, v.fps);
        vBytes += kbps * 1000.0 / 8.0;
        e.encodeUnits += encodeUnitsFor(v, in.hwEncode);
    }
    double aBytes = 0;
    for (const auto& a : in.audio) {
        if (a.compressedKbps > 0) aBytes += a.compressedKbps * 1000.0 / 8.0;
        else aBytes += static_cast<double>(a.sampleRate) * a.channels * (std::clamp(a.bitsPerSample, 16, 32) / 8);
    }
    e.videoMBps = vBytes / 1e6;
    e.audioMBps = aBytes / 1e6;
    e.programMBps = in.programKbps * 1000.0 / 8.0 / 1e6;
    e.totalMBps = e.videoMBps + e.audioMBps + e.programMBps;
    e.gbPerHour = e.totalMBps * 3600.0 / 1000.0;

    VideoPlan ref;
    e.baseUnits = std::max(0, in.baseEncodes) * encodeUnitsFor(ref, in.hwEncode);
    e.budgetUnits = budgetUnitsFor(in.tier);
    e.loadPercent = e.budgetUnits > 0 ? (e.encodeUnits + e.baseUnits) / e.budgetUnits * 100.0 : 0.0;
    if (!in.video.empty()) e.maxVideoTracksSafe = maxSafeVideoTracks(in.tier, in.hwEncode, in.video.front(), in.baseEncodes);

    // CPU / encoder verdict: Ok up to the budget, Warning up to 1.5x, Blocked beyond (override -> Warning).
    const double used = e.encodeUnits + e.baseUnits;
    if (e.videoTracks == 0) e.cpuVerdict = core::BudgetVerdict::Ok;
    else if (used <= e.budgetUnits + 1e-9) e.cpuVerdict = core::BudgetVerdict::Ok;
    else if (used <= e.budgetUnits * 1.5 + 1e-9) e.cpuVerdict = core::BudgetVerdict::Warning;
    else e.cpuVerdict = in.expertOverride ? core::BudgetVerdict::Warning : core::BudgetVerdict::Blocked;

    if (e.cpuVerdict == core::BudgetVerdict::Warning)
        e.notes.push_back("Carga de codificação estimada em " + fmt1(e.loadPercent) + "% do limite seguro deste computador (nível " +
                          core::tierName(in.tier) + "): podem ocorrer quadros perdidos nas gravações ISO; a saída ao vivo é preservada.");
    if (e.cpuVerdict == core::BudgetVerdict::Blocked)
        e.notes.push_back("Carga de codificação estimada em " + fmt1(e.loadPercent) + "% do limite seguro (nível " +
                          core::tierName(in.tier) + "): reduza câmeras/resolução ou ative o modo avançado (por sua conta e risco).");
    if (e.videoTracks > 0 && e.maxVideoTracksSafe >= 0)
        e.notes.push_back("Neste computador, até " + std::to_string(e.maxVideoTracksSafe) + " câmera(s) ISO nesse formato cabem na estimativa segura.");
    if (e.videoTracks + std::max(0, in.baseEncodes) > 3)
        e.notes.push_back("Placas de vídeo GeForce (NVIDIA) limitam as codificações H.264 simultâneas por hardware (o limite varia com o driver): "
                          "acima disso o codificador cai para software ou falha.");

    // Disk verdict.
    if (in.freeBytes > 0 && e.totalMBps > 0) {
        const double usable = in.freeBytes > in.minFreeBytes ? static_cast<double>(in.freeBytes - in.minFreeBytes) : 0.0;
        e.minutesOfDisk = usable / (e.totalMBps * 1e6) / 60.0;
        if (e.minutesOfDisk < 5.0) e.diskVerdict = in.expertOverride ? core::BudgetVerdict::Warning : core::BudgetVerdict::Blocked;
        else if (e.minutesOfDisk < 60.0) e.diskVerdict = core::BudgetVerdict::Warning;
        if (e.diskVerdict != core::BudgetVerdict::Ok)
            e.notes.push_back("Espaço em disco para cerca de " + fmt1(e.minutesOfDisk) + " min nesta taxa (" + fmt1(e.totalMBps) + " MB/s).");
    }
    e.verdict = worse(e.cpuVerdict, e.diskVerdict);
    return e;
}

}  // namespace hb::multitrack
