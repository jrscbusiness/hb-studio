#pragma once
// Publishes mixer levels on the EventBus as topic "audio.levels", rate-limited.
// Call tick() from a UI/timer thread (NEVER from the audio thread: the bus
// takes a mutex and allocates). Time is injected for deterministic tests.
//
// Fields (all strings, dB/LUFS with one decimal, "-inf" for silence):
//   program.peak.l/.r  program.hold.l/.r  program.rms.l/.r  program.truePeak.l/.r
//   program.lufs.m  program.lufs.s  program.limiterGr
//   channels (count), ch.<i>.name, ch.<i>.peak (max of L/R), ch.<i>.mute, ch.<i>.solo
#include "audio/mixer.hpp"
#include "core/event_bus.hpp"

namespace hb::audio {

class LevelPublisher {
public:
    LevelPublisher(const Mixer& mixer, hb::core::EventBus& bus, double maxHz = 10.0);
    // Publishes if at least 1/maxHz seconds passed since the last publish.
    bool tick(double nowSeconds);
    static std::string formatDb(float v);

private:
    const Mixer& mixer_;
    hb::core::EventBus& bus_;
    double minInterval_;
    double last_ = -1e30;
};

}  // namespace hb::audio
