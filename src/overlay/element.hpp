#pragma once
// The six overlay elements. Type names are a STABLE contract (scene templates reference them as plain
// data {type, params}):  "timer", "scoreboard", "qr", "lowerthird", "captions", "chatpin".
#include <cstdint>
#include <mutex>
#include <string>

#include "core/json.hpp"
#include "overlay/layer.hpp"
#include "overlay/timing.hpp"

namespace hb::overlay {

struct ElementConfig {
    // identity
    std::string id, type, name;
    // placement (normalized anchor + pivot) and stacking
    double x = 0.5, y = 0.5, pivotX = 0.5, pivotY = 0.5;
    int z = 0;
    // look. `size` is the main text height as a fraction of the canvas height (resolution independent).
    double size = 0.06;
    std::string font = "Segoe UI";
    bool bold = true;
    std::uint32_t textColor = 0xFFFFFF, bgColor = 0x000000, accent = 0x1F6BFF;
    double bgAlpha = 0.6, opacity = 1.0;
    // animation
    std::string animIn = "fade", animOut = "fade";  // none | fade | slide-left | slide-right | slide-up | slide-down
    int animMs = 400;
    // timer (also the optional clock of the scoreboard)
    std::string mode = "down";     // down | up
    int durationSec = 300;
    std::string format = "auto";   // auto | mm:ss | hh:mm:ss
    bool blinkAtZero = true;
    std::string label;             // small text above the timer / scoreboard
    // scoreboard
    std::string teamA = "CASA", teamB = "VISITANTE", period;
    std::uint32_t colorA = 0xC62828, colorB = 0x1565C0;
    int scoreA = 0, scoreB = 0;
    bool showClock = false;
    // qr
    std::string qrText, qrCaption;
    std::string qrEcc = "M";       // L | M | Q | H
    double qrSize = 0.28;          // QR height as a fraction of the canvas height
    std::uint32_t qrFg = 0x000000, qrBg = 0xFFFFFF;
    // lower third / chat pin card
    std::string title, subtitle;
    // captions / chat pin
    double holdSec = 4.0;          // captions: silence before fading out; chatpin: auto-hide delay (0 = stay)
    int maxLines = 2;
    double widthPct = 0.8;         // max text width as a fraction of the canvas width
};

bool isValidType(const std::string& type);
ElementConfig defaultConfig(const std::string& type);
void sanitize(ElementConfig& c);  // clamps every value, repairs strings; never throws
core::json::Value toJson(const ElementConfig& c);
// Starts from defaultConfig(type) (type taken from the JSON, or `fallbackType`), applies present fields.
bool fromJson(const core::json::Value& v, ElementConfig& out, const std::string& fallbackType = "");

class Element : public Layer {
public:
    explicit Element(ElementConfig cfg);

    // ---- Layer ----
    std::string id() const override;
    bool visible() const override;
    int zOrder() const override;
    Placement placement() const override;
    bool active(const RenderContext& ctx) const override;
    void render(video::Frame& canvas, const RenderContext& ctx) override;

    // ---- control ----
    ElementConfig config() const;
    void applyConfig(const ElementConfig& cfg, std::int64_t now);
    void setOn(bool on, std::int64_t now);
    bool isOn(std::int64_t now) const;
    // cmd: start | pause | reset | toggle | set (arg = seconds, resets) | add (arg = +/- seconds)
    bool timerCommand(const std::string& cmd, double arg, std::int64_t now);
    // who: "a" | "b" (delta added to the score), "reset", "swap"
    bool scoreCommand(const std::string& who, int delta, std::int64_t now);
    void setPeriod(const std::string& text);
    // Feeds ("caption.text"): partial results replace the line in progress, final ones are committed.
    void onCaption(const std::string& text, bool final, std::int64_t now);
    // Feeds ("overlay.chatPin"): shows the card and arms the auto-hide timer.
    void onChatPin(const std::string& author, const std::string& text, const std::string& platform, std::int64_t now);
    // Live data for the UI: timer text, running flag, remaining etc.
    void liveFields(std::int64_t now, std::string& timerText, bool& running, bool& finished) const;
    std::uint64_t rasterCount() const;  // how many times the card was rebuilt (tests / perf counters)

private:
    double contentAlpha(std::int64_t now) const;  // callers hold mutex_
    bool buildCard(const RenderContext& ctx, std::int64_t now, std::string& key);  // callers hold mutex_

    mutable std::mutex mutex_;
    ElementConfig cfg_;
    Timer timer_;
    Animator anim_;
    // captions
    std::string committed_, partial_;
    std::int64_t lastCaptionMs_ = -1;
    // chat pin
    std::string chatAuthor_, chatText_, chatPlatform_;
    std::int64_t autoHideAt_ = -1;
    // cached rasterized card
    std::string cardKey_;
    Bitmap card_;
    std::uint64_t rasters_ = 0;
};

}  // namespace hb::overlay
