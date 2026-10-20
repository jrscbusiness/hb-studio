#include "audio/level_publisher.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace hb::audio {

LevelPublisher::LevelPublisher(const Mixer& mixer, hb::core::EventBus& bus, double maxHz)
    : mixer_(mixer), bus_(bus), minInterval_(1.0 / std::max(0.1, maxHz)) {}

std::string LevelPublisher::formatDb(float v) {
    if (std::isinf(v) || std::isnan(v)) return v > 0 ? "inf" : "-inf";
    char b[32];
    std::snprintf(b, sizeof b, "%.1f", static_cast<double>(v));
    return b;
}

bool LevelPublisher::tick(double now) {
    if (now - last_ < minInterval_) return false;
    last_ = now;
    const ProgramLevels p = mixer_.programLevels();
    hb::core::Event e;
    e.topic = "audio.levels";
    auto& f = e.fields;
    f["program.peak.l"] = formatDb(p.peakDb[0]);
    f["program.peak.r"] = formatDb(p.peakDb[1]);
    f["program.hold.l"] = formatDb(p.holdDb[0]);
    f["program.hold.r"] = formatDb(p.holdDb[1]);
    f["program.rms.l"] = formatDb(p.rmsDb[0]);
    f["program.rms.r"] = formatDb(p.rmsDb[1]);
    f["program.truePeak.l"] = formatDb(p.truePeakDb[0]);
    f["program.truePeak.r"] = formatDb(p.truePeakDb[1]);
    f["program.lufs.m"] = formatDb(p.momentaryLufs);
    f["program.lufs.s"] = formatDb(p.shortTermLufs);
    f["program.limiterGr"] = formatDb(p.limiterReductionDb);
    f["channels"] = std::to_string(mixer_.channelCount());
    for (std::size_t i = 0; i < mixer_.channelCount(); ++i) {
        const std::string k = "ch." + std::to_string(i) + ".";
        const ChannelLevels l = mixer_.channelLevels(i);
        f[k + "name"] = mixer_.channelName(i);
        f[k + "peak"] = formatDb(std::max(l.peakDb[0], l.peakDb[1]));
        f[k + "mute"] = mixer_.muted(i) ? "1" : "0";
        f[k + "solo"] = mixer_.soloed(i) ? "1" : "0";
    }
    bus_.publish(e);
    return true;
}

}  // namespace hb::audio
