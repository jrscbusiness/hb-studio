#include <algorithm>
#include <cmath>

#include "video/source.hpp"

namespace hb::video {

namespace {
// 5x7 digit glyphs, one row per byte, bit 4 = leftmost column.
const std::uint8_t kGlyph[10][7] = {
    {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}, {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E},
    {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}, {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E},
    {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}, {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E},
    {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}, {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},
    {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}, {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C},
};

struct Rgb { float r, g, b; };

Rgb hsv(float h, float s, float v) {
    const float c = v * s;
    const float hp = std::fmod(h, 360.f) / 60.f;
    const float x = c * (1.f - std::fabs(std::fmod(hp, 2.f) - 1.f));
    float r = 0, g = 0, b = 0;
    if (hp < 1) { r = c; g = x; } else if (hp < 2) { r = x; g = c; } else if (hp < 3) { g = c; b = x; }
    else if (hp < 4) { g = x; b = c; } else if (hp < 5) { r = x; b = c; } else { r = c; b = x; }
    const float m = v - c;
    return {(r + m) * 255.f, (g + m) * 255.f, (b + m) * 255.f};
}

inline std::uint8_t u8(float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.f, 255.f)); }
}  // namespace

void SyntheticSource::render(Frame& f, std::uint64_t index) const {
    const int W = f.width, H = f.height;
    if (W <= 0 || H <= 0 || f.data.size() < static_cast<std::size_t>(f.stride) * H) return;

    const int n = ((number_ % 360) + 360) % 360;
    const float hue = static_cast<float>((n - 1 + 360) * 43 % 360) + 200.f;  // blue-leaning palette
    const Rgb top = hsv(hue, 0.45f, 0.98f), bot = hsv(hue, 0.85f, 0.42f);

    const float t = static_cast<float>(index) / 30.f;
    const float cx = W * (0.5f + 0.33f * std::sin(t * (0.9f + 0.17f * number_)));
    const float cy = H * (0.5f + 0.22f * std::sin(t * (1.5f + 0.11f * number_) + number_));
    const float r = H * 0.11f, r2 = r * r;

    const int gridX = std::max(1, W / 16), gridY = std::max(1, H / 9);
    const int s = std::max(1, H / 13);  // digit pixel size
    const int digit = ((number_ % 10) + 10) % 10;
    const int ox = W / 2 - (5 * s) / 2, oy = H / 2 - (7 * s) / 2;
    const int barY = H - std::max(2, H / 36);
    const int barW = static_cast<int>(W * ((index % 90) / 90.f));

    for (int y = 0; y < H; ++y) {
        const float k = H > 1 ? static_cast<float>(y) / (H - 1) : 0.f;
        const float br = top.r + (bot.r - top.r) * k, bg = top.g + (bot.g - top.g) * k, bb = top.b + (bot.b - top.b) * k;
        const bool gridRow = (y % gridY) == 0;
        std::uint8_t* row = f.data.data() + static_cast<std::size_t>(y) * f.stride;
        for (int x = 0; x < W; ++x) {
            float R = br, G = bg, B = bb;
            if (gridRow || (x % gridX) == 0) { R += 28; G += 28; B += 28; }
            const float dx = x - cx, dy = y - cy;
            if (dx * dx + dy * dy < r2) { R = R * 0.25f + 255 * 0.75f; G = G * 0.25f + 255 * 0.75f; B = B * 0.25f + 255 * 0.75f; }
            if (y >= barY && x < barW) { R = G = B = 255; }
            const int gx = (x - ox) / s, gy = (y - oy) / s;
            if (x >= ox && y >= oy && gx < 5 && gy < 7 && (kGlyph[digit][gy] >> (4 - gx)) & 1) { R = G = B = 255; }
            std::uint8_t* p = row + x * 4;
            p[0] = u8(B); p[1] = u8(G); p[2] = u8(R); p[3] = 255;
        }
    }
}

}  // namespace hb::video
