#pragma once
// The compact state pushed to remote devices. By construction this struct has NO field that could
// hold a secret (no stream keys, server URLs, API keys, file paths): the host fills it from its own
// state and toJson() is the only way it reaches the network.
#include <string>
#include <vector>

namespace hb::remote {

struct RemoteSource {
    std::string name;    // "CAM 1", device label...
    std::string status;  // online | connecting | nosignal | error | test | off
};
struct RemoteStrip {
    std::string name;
    double faderDb = 0;
    bool mute = false, solo = false;
    double peakDb = -120;
};
struct RemoteDest {
    std::string id, name, platform;  // platform is only a label ("youtube", "twitch", "custom"...)
    bool running = false, publishing = false;
};
struct RemoteState {
    int pvw = 0, pgm = 0;
    bool transitioning = false;
    double progress = 0;
    bool recording = false;
    double masterPeakDb = -120;
    std::vector<RemoteSource> sources;
    std::vector<RemoteStrip> strips;
    std::vector<RemoteDest> dests;
};

// {"t":"s",...}. Strings are JSON-escaped and length-capped; non-finite numbers become safe defaults.
// Levels are rounded to whole dB (so an idle mixer does not produce a different message every tick).
std::string toJson(const RemoteState& s);

}  // namespace hb::remote
