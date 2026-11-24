#pragma once
// Chroma / AI background removal: shared value types (portable, no platform code).
// See docs/CHROMA.md for the alpha convention (BGRA8, PREMULTIPLIED) and the limits of each mode.
#include <cstdint>
#include <string>
#include <vector>

namespace hb::chroma {

// An 8-bit single-channel matte: 255 = foreground (person), 0 = background.
struct Mask {
    int width = 0, height = 0;
    std::vector<std::uint8_t> data;   // width*height, tightly packed
    bool valid() const {
        return width > 0 && height > 0 && data.size() >= static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    }
    void resize(int w, int h, std::uint8_t fill = 0) {
        width = w;
        height = h;
        data.assign(static_cast<std::size_t>(w > 0 ? w : 0) * static_cast<std::size_t>(h > 0 ? h : 0), fill);
    }
};

enum class Mode { Off, Green, Blue, Ai };
enum class Quality { Low, Medium, High };
enum class ReplKind { Color, Blur, Image, Transparent };

// Everything the operator can configure for one slot. Plain data; serialised to a compact
// "k=v;k=v" string (no tabs/newlines) so it fits one field of source_assignments.
struct Settings {
    Mode mode = Mode::Off;
    int tolerance = 45;     // 0..100. Classic: how far from the key colour still counts as screen. AI: matte threshold centre.
    int softness = 25;      // 0..100. Width of the transition band (edge feather).
    int spill = 50;         // 0..100. Classic only: strength of green/blue spill suppression.
    Quality quality = Quality::Medium;   // AI only: inference rate / post-processing effort
    ReplKind repl = ReplKind::Color;
    std::uint32_t color = 0x101010;      // 0xRRGGBB, used by ReplKind::Color
    int blur = 40;                       // 0..100, strength of ReplKind::Blur
    std::string image;                   // path, used by ReplKind::Image (sanitised, <= 400 bytes)

    bool operator==(const Settings&) const = default;
};

const char* modeName(Mode m);
const char* qualityName(Quality q);
const char* replName(ReplKind r);

// Parsing never throws; unknown keys/values are ignored (forward compatible), numbers are clamped.
Settings parseSettings(const std::string& text);
std::string serializeSettings(const Settings& s);
// Applies a single "key","value" pair (the UI's live slider commands). False if key/value is invalid.
bool setField(Settings& s, const std::string& key, const std::string& value);

// %XX escaping for ';', '=', '%', '|', control chars (keeps path/values safe inside the compact string).
std::string pctEncode(const std::string& s);
std::string pctDecode(const std::string& s);

// Cost knobs derived from the quality level (AI mode).
struct QualityParams {
    int minIntervalMs;    // minimum time between inference starts
    int erode;            // mask-resolution erode passes (shrinks halo)
    int feather;          // mask-resolution box-blur radius
    int emaNew;           // 1..256 weight of the new mask in temporal smoothing
};
QualityParams qualityParams(Quality q);

}  // namespace hb::chroma
