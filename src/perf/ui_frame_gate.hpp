#pragma once
// Cuts the cost of the UI frame path (render -> JPEG -> base64 -> WebView) without touching the Program output:
//   * UiFrameGate: skips encoding/sending a UI frame whose pixels did not change since the last one sent for the
//     same id (cheap 64-bit hash of the already downscaled frame). An unchanged frame is still re-sent every
//     `keepAliveSec` so a reloaded page never stays blank. Live cameras carry sensor noise, so this mainly helps
//     static content (NO SIGNAL / test patterns, still scenes, idle tiles) - see docs/DESEMPENHO.md.
//   * UiFramePolicy: the UI reports the active tab; on tabs that do not show the monitors/tiles the feeder sends
//     them at a much lower rate. Hot tabs (PRODUÇÃO, GRÁFICOS, CÂMERAS) keep the full rate.
// Both are portable and thread safe where noted.
#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>

#include "video/program_output.hpp"

namespace hb::perf {

std::uint64_t hashFrame(const video::Frame& f);   // covers width, height and every pixel row

class UiFrameGate {
public:
    explicit UiFrameGate(double keepAliveSec = 1.0) : keepAliveSec_(keepAliveSec) {}
    // True when the frame must be encoded and sent. Single-threaded (feeder thread).
    bool shouldSend(const std::string& id, const video::Frame& f, double nowSec);
    void reset() { state_.clear(); }   // next frame of every id is sent (tab change, page reload)
    std::uint64_t sent() const { return sent_; }
    std::uint64_t suppressed() const { return suppressed_; }

private:
    struct State { std::uint64_t hash = 0; double lastSent = 0; };
    double keepAliveSec_;
    std::map<std::string, State> state_;
    std::uint64_t sent_ = 0, suppressed_ = 0;
};

class UiFramePolicy {
public:
    // `tab` is the UI view id ("prod", "gfx", "cam", "cfg", ...). Thread safe.
    void setActiveTab(const std::string& tab);
    std::string activeTab() const;
    bool hot() const { return hot_.load(); }
    // Feeder tick counter `tick` (15 Hz): is the PGM/PVW monitor due?
    bool monitorDue(std::uint64_t tick) const { return hot_.load() || tick % 4 == 0; }
    // Input/scene tile `index`: the feeder already alternates parity (index % 2 == tick % 2); on cold tabs each
    // tile is additionally sent once every 8 ticks.
    bool tileDue(std::uint64_t tick, int index) const {
        return hot_.load() || ((tick / 2) % 4) == (static_cast<std::uint64_t>(index) / 2) % 4;
    }
    static bool isHotTab(const std::string& tab);

private:
    mutable std::mutex mutex_;
    std::string tab_ = "prod";
    std::atomic<bool> hot_{true};
};

}  // namespace hb::perf
