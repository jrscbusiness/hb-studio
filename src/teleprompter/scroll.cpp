#include "teleprompter/scroll.hpp"

#include <algorithm>
#include <cmath>

namespace hb::teleprompter {

const char* runStateName(RunState s) {
    switch (s) {
        case RunState::Stopped: return "stopped";
        case RunState::Countdown: return "countdown";
        case RunState::Playing: return "playing";
        case RunState::Paused: return "paused";
        case RunState::Finished: return "finished";
    }
    return "stopped";
}

const char* speedModeName(SpeedMode m) { return m == SpeedMode::Wpm ? "wpm" : "pxs"; }

double wpmToPxPerSec(double wpm, int words, double contentPx) {
    if (!(wpm > 0) || words <= 0 || !(contentPx > 0) || !std::isfinite(wpm) || !std::isfinite(contentPx)) return 0;
    return wpm / 60.0 * contentPx / words;
}

double pxPerSecToWpm(double pxs, int words, double contentPx) {
    if (!(pxs > 0) || words <= 0 || !(contentPx > 0) || !std::isfinite(pxs) || !std::isfinite(contentPx)) return 0;
    return pxs * 60.0 * words / contentPx;
}

double clampWpm(double v) { return std::isfinite(v) ? std::clamp(v, kMinWpm, kMaxWpm) : kMinWpm; }
double clampPxPerSec(double v) { return std::isfinite(v) ? std::clamp(v, kMinPxPerSec, kMaxPxPerSec) : kMinPxPerSec; }

double ScrollState::rate() const {
    if (mode_ == SpeedMode::Wpm) return words_ > 0 ? wpm_ / 60.0 / words_ : 0.0;
    return contentPx_ > 0 ? pxs_ / contentPx_ : 0.0;
}

void ScrollState::settle(double now) {
    if (state_ == RunState::Countdown && now >= countdownEnd_) {
        state_ = RunState::Playing;
        anchorT_ = countdownEnd_;   // exact end of the countdown: no drift from late polling
    }
    if (state_ == RunState::Playing) {
        const double r = rate();
        const double p = pos_ + r * (now - anchorT_);
        anchorT_ = now;
        if (p >= 1.0) {
            pos_ = 1.0;
            state_ = RunState::Finished;
        } else {
            pos_ = std::max(0.0, p);
        }
    }
}

// Must be called right after settle(): makes `pos_` valid at `now` before a rate change.
void ScrollState::reanchor(double now) { anchorT_ = now; }

void ScrollState::setScriptWords(int words) {
    settle(clock_->now());
    words_ = std::max(0, words);
}

void ScrollState::setLayout(double contentPx, double viewportPx) {
    if (!std::isfinite(contentPx) || !std::isfinite(viewportPx) || contentPx <= 0 || viewportPx <= 0) return;
    settle(clock_->now());
    contentPx_ = contentPx;
    viewportPx_ = viewportPx;
}

void ScrollState::setMode(SpeedMode m) {
    if (m == mode_) return;
    settle(clock_->now());
    if (m == SpeedMode::PxPerSec) {
        const double v = wpmToPxPerSec(wpm_, words_, contentPx_);
        if (v > 0) pxs_ = clampPxPerSec(v);
    } else {
        const double v = pxPerSecToWpm(pxs_, words_, contentPx_);
        if (v > 0) wpm_ = clampWpm(v);
    }
    mode_ = m;
}

void ScrollState::setWpm(double v) {
    if (!std::isfinite(v)) return;
    settle(clock_->now());
    wpm_ = clampWpm(v);
}

void ScrollState::setPxPerSec(double v) {
    if (!std::isfinite(v)) return;
    settle(clock_->now());
    pxs_ = clampPxPerSec(v);
}

void ScrollState::setSpeed(double v) {
    if (mode_ == SpeedMode::Wpm) setWpm(v);
    else setPxPerSec(v);
}

void ScrollState::nudgeSpeed(double delta) {
    if (!std::isfinite(delta)) return;
    setSpeed((mode_ == SpeedMode::Wpm ? wpm_ : pxs_) + delta);
}

void ScrollState::setCountdown(int seconds) { countdown_ = std::clamp(seconds, 0, kMaxCountdownSec); }

void ScrollState::play() {
    const double now = clock_->now();
    settle(now);
    if (state_ == RunState::Playing || state_ == RunState::Countdown) return;
    if (state_ == RunState::Finished) pos_ = 0;
    if (countdown_ > 0 && pos_ <= 1e-9) {
        state_ = RunState::Countdown;
        countdownEnd_ = now + countdown_;
    } else {
        state_ = RunState::Playing;
        reanchor(now);
    }
}

void ScrollState::pause() {
    settle(clock_->now());
    if (state_ == RunState::Playing || state_ == RunState::Countdown) state_ = RunState::Paused;
}

void ScrollState::toggle() {
    settle(clock_->now());
    if (state_ == RunState::Playing || state_ == RunState::Countdown) pause();
    else play();
}

void ScrollState::restart() {
    pos_ = 0;
    state_ = RunState::Stopped;
}

void ScrollState::seek(double f) {
    if (!std::isfinite(f)) return;
    const double now = clock_->now();
    settle(now);
    pos_ = std::clamp(f, 0.0, 1.0);
    if (state_ == RunState::Playing) {
        reanchor(now);
        if (pos_ >= 1.0) state_ = RunState::Finished;
    } else if (state_ == RunState::Finished) {
        state_ = pos_ >= 1.0 ? RunState::Finished : RunState::Paused;
    } else if (state_ == RunState::Countdown && pos_ > 1e-9) {
        state_ = RunState::Paused;   // jumping into the text cancels the "get ready" countdown
    } else if (state_ == RunState::Stopped && pos_ > 1e-9) {
        state_ = RunState::Paused;
    }
}

Snapshot ScrollState::snapshot() {
    const double now = clock_->now();
    settle(now);
    Snapshot s;
    s.state = state_;
    s.pos = pos_;
    s.mode = mode_;
    s.countdownSec = countdown_;
    s.words = words_;
    s.contentPx = contentPx_;
    s.viewportPx = viewportPx_;
    const double r = rate();
    s.rate = state_ == RunState::Playing ? r : 0.0;
    if (mode_ == SpeedMode::Wpm) {
        s.wpm = wpm_;
        s.pxPerSec = wpmToPxPerSec(wpm_, words_, contentPx_);
    } else {
        s.pxPerSec = pxs_;
        s.wpm = pxPerSecToWpm(pxs_, words_, contentPx_);
    }
    if (state_ == RunState::Countdown) s.countdownLeft = std::max(0.0, countdownEnd_ - now);
    if (r > 0) {
        s.totalSec = 1.0 / r;
        s.remainingSec = (1.0 - pos_) / r + s.countdownLeft;
    }
    if (state_ == RunState::Finished) s.remainingSec = 0;
    return s;
}

}  // namespace hb::teleprompter
