#pragma once
#include <chrono>
#include <string>

// Teleprompter scroll model: speed conversions and the play/pause/seek/countdown state machine.
// Position is always a fraction 0..1 of the script (0 = reading line on the first line, 1 = the
// last line has passed the reading line), so it is independent of any window's size: every view
// (operator preview, presenter window, a future remote agent) maps the fraction to its own pixels.
// The host owns the state; views extrapolate from (pos, rate) and are re-synced by each snapshot,
// so they cannot drift apart.
namespace hb::teleprompter {

// Time source injected for deterministic tests.
class Clock {
public:
    virtual ~Clock() = default;
    virtual double now() const = 0;   // seconds, monotonic
};

class SteadyClockSource final : public Clock {
public:
    double now() const override {
        return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }
};

class ManualClock final : public Clock {
public:
    double now() const override { return t_; }
    void advance(double seconds) { t_ += seconds; }
    void set(double t) { t_ = t; }
private:
    double t_ = 1000.0;
};

enum class SpeedMode { Wpm, PxPerSec };
enum class RunState { Stopped, Countdown, Playing, Paused, Finished };

constexpr double kMinWpm = 20, kMaxWpm = 400;
constexpr double kMinPxPerSec = 10, kMaxPxPerSec = 800;
constexpr int kMaxCountdownSec = 30;

const char* runStateName(RunState s);
const char* speedModeName(SpeedMode m);

// Words-per-minute <-> pixels-per-second for a script of `words` words laid out `contentPx` tall.
// The model assumes words are spread evenly over the height (an approximation: blank lines and
// short lines make local speed vary by a few percent). Returns 0 when the inputs are not usable.
double wpmToPxPerSec(double wpm, int words, double contentPx);
double pxPerSecToWpm(double pxPerSec, int words, double contentPx);

double clampWpm(double wpm);
double clampPxPerSec(double pxs);

struct Snapshot {
    RunState state = RunState::Stopped;
    double pos = 0;             // 0..1
    double rate = 0;            // fraction per second while Playing, else 0 (views extrapolate with this)
    double countdownLeft = 0;   // seconds, only in Countdown
    double remainingSec = 0;    // estimated time to the end incl. countdown; 0 if unknown
    double totalSec = 0;        // estimated reading time of the whole script; 0 if unknown
    SpeedMode mode = SpeedMode::Wpm;
    double wpm = 0;             // effective values in BOTH units (the non-master one is derived)
    double pxPerSec = 0;
    int countdownSec = 0;       // configured countdown
    int words = 0;
    double contentPx = 0, viewportPx = 0;
};

class ScrollState {
public:
    explicit ScrollState(const Clock* clock) : clock_(clock) {}

    void setScriptWords(int words);
    void setLayout(double contentPx, double viewportPx);   // ignored unless both are finite and > 0
    void setMode(SpeedMode m);                             // keeps the effective speed
    void setWpm(double wpm);                               // also switches nothing: sets the wpm value
    void setPxPerSec(double pxs);
    void setSpeed(double v);                               // in the current mode's unit
    void nudgeSpeed(double delta);                         // current mode's unit, clamped
    void setCountdown(int seconds);

    void play();      // from Finished restarts from 0; at pos 0 with countdown > 0 enters Countdown
    void pause();     // Playing/Countdown -> Paused (a countdown in progress is cancelled)
    void toggle();
    void restart();   // pos 0, Stopped
    void seek(double fraction);   // NaN ignored, clamped to 0..1

    Snapshot snapshot();          // settles time-driven transitions (countdown end, finish) first

    SpeedMode mode() const { return mode_; }
    double wpm() const { return wpm_; }
    double pxPerSec() const { return pxs_; }
    int countdown() const { return countdown_; }

private:
    void settle(double now);
    double rate() const;   // fraction/s at the current speed (0 if script/layout unknown)
    void reanchor(double now);

    const Clock* clock_;
    RunState state_ = RunState::Stopped;
    double pos_ = 0;
    double anchorT_ = 0;
    double countdownEnd_ = 0;
    SpeedMode mode_ = SpeedMode::Wpm;
    double wpm_ = 150, pxs_ = 80;
    int countdown_ = 3;
    int words_ = 0;
    double contentPx_ = 0, viewportPx_ = 0;
};

}  // namespace hb::teleprompter
