#pragma once
// Strict allowlist of what a remote device may ask the switcher to do.
//
// A remote message is JSON ({"t":"cmd","c":"cut"}, {"t":"cmd","c":"fader","ch":2,"db":-6.5}, ...).
// translate() never forwards client text: every output string is REBUILT from typed, range-checked
// values, so a '|' or any other control character in a client string can never inject another
// command into the host's "cmd|arg|arg" protocol. Anything not listed here is rejected.
//
// Allowed commands (the whole remote surface):
//   cut | auto{ms} | pvw{i} | pgm{i} | tbar{v} | mute{ch,on} | solo{ch,on} | fader{ch,db}
//   rec{on} | xmit{id,on}  (one destination; no "all") | tp{a: play|pause|speed|seek, v}
#include <optional>
#include <string>

#include "core/json.hpp"

namespace hb::remote {

struct CommandLimits {
    int maxSources = 16;   // preview/program index range [0, maxSources)
    int maxChannels = 16;  // audio channel range [0, maxChannels)
    int minAutoMs = 100, maxAutoMs = 5000, defaultAutoMs = 500;
};

struct Translation {
    bool ok = false;
    std::string hostCommand;  // e.g. "dissolve|500"  (valid only if ok)
    std::string error;        // short machine-readable reason when !ok
};

Translation translate(const core::json::Value& msg, const CommandLimits& limits = {});

}  // namespace hb::remote
