#pragma once
// Factories of the built-in effects (registered by registerBuiltinEffects()).
#include <memory>

#include "audio/effect.hpp"

namespace hb::audio::fx {

std::unique_ptr<Effect> makeGain();
std::unique_ptr<Effect> makePhase();
std::unique_ptr<Effect> makeMono();
std::unique_ptr<Effect> makeDelay();
std::unique_ptr<Effect> makeHighpass();
std::unique_ptr<Effect> makeLowpass();
std::unique_ptr<Effect> makeEq3();
std::unique_ptr<Effect> makeGate();
std::unique_ptr<Effect> makeCompressor();
std::unique_ptr<Effect> makeLimiter();
std::unique_ptr<Effect> makeDeesser();

// Look-ahead (= reported latency) of hb.limiter, in frames, for a sample rate.
int limiterLatencyFrames(int sampleRate) noexcept;

}  // namespace hb::audio::fx
