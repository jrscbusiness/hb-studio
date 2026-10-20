#include <algorithm>
#include <cmath>
#include <cstring>

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

std::shared_ptr<const Frame> SyntheticSource::background(int W, int H) const {
    {
        std::lock_guard lock(cacheMutex_);
        auto it = cache_.find({W, H});
        if (it != cache_.end()) return it->second;
    }
    auto bg = std::make_shared<Frame>();
    bg->width = W;
    bg->height = H;
    bg->stride = W * 4;
    bg->data.resize(static_cast<std::size_t>(bg->stride) * H);

    const int n = ((number_ % 360) + 360) % 360;
    const float hue = static_cast<float>((n - 1 + 360) * 43 % 360) + 200.f;  // blue-leaning palette
    const Rgb top = hsv(hue, 0.45f, 0.98f), bot = hsv(hue, 0.85f, 0.42f);
    const int gridX = std::max(1, W / 16), gridY = std::max(1, H / 9);
    for (int y = 0; y < H; ++y) {
        const float k = H > 1 ? static_cast<float>(y) / (H - 1) : 0.f;
        const float br = top.r + (bot.r - top.r) * k, bg2 = top.g + (bot.g - top.g) * k, bb = top.b + (bot.b - top.b) * k;
        const bool gridRow = (y % gridY) == 0;
        std::uint8_t* row = bg->data.data() + static_cast<std::size_t>(y) * bg->stride;
        const std::uint8_t pr = u8(br), pg = u8(bg2), pb = u8(bb);
        const std::uint8_t gr = u8(br + 28), gg = u8(bg2 + 28), gb = u8(bb + 28);
        for (int x = 0; x < W; ++x) {
            const bool grid = gridRow || (x % gridX) == 0;
            std::uint8_t* p = row + x * 4;
            p[0] = grid ? gb : pb; p[1] = grid ? gg : pg; p[2] = grid ? gr : pr; p[3] = 255;
        }
    }
    std::lock_guard lock(cacheMutex_);
    if (cache_.size() >= 6) cache_.clear();  // sizes in use: Program, monitors, tiles (+ tests)
    cache_[{W, H}] = bg;
    return bg;
}

void SyntheticSource::render(Frame& f, std::uint64_t index) const {
    const int W = f.width, H = f.height;
    if (W <= 0 || H <= 0 || f.data.size() < static_cast<std::size_t>(f.stride) * H || f.stride != W * 4) return;

    const auto bg = background(W, H);
    std::memcpy(f.data.data(), bg->data.data(), static_cast<std::size_t>(W) * 4 * H);

    // moving circle (bounding box only)
    const float t = static_cast<float>(index) / 30.f;
    const float cx = W * (0.5f + 0.33f * std::sin(t * (0.9f + 0.17f * number_)));
    const float cy = H * (0.5f + 0.22f * std::sin(t * (1.5f + 0.11f * number_) + number_));
    const float r = H * 0.11f, r2 = r * r;
    const int y0 = std::max(0, static_cast<int>(cy - r) - 1), y1 = std::min(H - 1, static_cast<int>(cy + r) + 1);
    const int x0 = std::max(0, static_cast<int>(cx - r) - 1), x1 = std::min(W - 1, static_cast<int>(cx + r) + 1);
    for (int y = y0; y <= y1; ++y) {
        std::uint8_t* row = f.data.data() + static_cast<std::size_t>(y) * f.stride;
        for (int x = x0; x <= x1; ++x) {
            const float dx = x - cx, dy = y - cy;
            if (dx * dx + dy * dy < r2) {
                std::uint8_t* p = row + x * 4;
                for (int c = 0; c < 3; ++c) p[c] = static_cast<std::uint8_t>((p[c] + 3 * 255) / 4);
            }
        }
    }

    // progress bar along the bottom
    const int barY = H - std::max(2, H / 36);
    const int barW = static_cast<int>(W * ((index % 90) / 90.f));
    for (int y = barY; y < H; ++y)
        std::memset(f.data.data() + static_cast<std::size_t>(y) * f.stride, 255, static_cast<std::size_t>(std::min(barW, W)) * 4);

    // big source number in the centre
    const int s = std::max(1, H / 13);
    const int digit = ((number_ % 10) + 10) % 10;
    const int ox = W / 2 - (5 * s) / 2, oy = H / 2 - (7 * s) / 2;
    for (int gy = 0; gy < 7; ++gy)
        for (int gx = 0; gx < 5; ++gx) {
            if (!((kGlyph[digit][gy] >> (4 - gx)) & 1)) continue;
            for (int yy = oy + gy * s; yy < oy + (gy + 1) * s; ++yy) {
                if (yy < 0 || yy >= H) continue;
                for (int xx = ox + gx * s; xx < ox + (gx + 1) * s; ++xx) {
                    if (xx < 0 || xx >= W) continue;
                    std::uint8_t* p = f.data.data() + static_cast<std::size_t>(yy) * f.stride + xx * 4;
                    p[0] = p[1] = p[2] = 255;
                }
            }
        }
}

}  // namespace hb::video
