#include "chroma/compositor.hpp"

#include <algorithm>
#include <vector>

#include "video/frame_ops.hpp"

namespace hb::chroma {

namespace {
inline bool frameOk(const video::Frame& f) {
    return f.width > 0 && f.height > 0 && f.stride >= f.width * 4 &&
           f.data.size() >= static_cast<std::size_t>(f.stride) * static_cast<std::size_t>(f.height);
}
inline int div255(int v) { return (v + 127) / 255; }
}  // namespace

void compositeMatte(video::Frame& fg, const Mask& matte, const Background& bg, Spill spill, int spillAmount) {
    if (!frameOk(fg) || !matte.valid()) return;
    despill(fg, spill, spillAmount);

    const bool transparent = bg.kind == ReplKind::Transparent;
    const video::Frame* img = (bg.kind == ReplKind::Blur || bg.kind == ReplKind::Image) &&
                                      bg.image && frameOk(*bg.image) && bg.image->width == fg.width &&
                                      bg.image->height == fg.height
                                  ? bg.image
                                  : nullptr;
    const int cr = static_cast<int>((bg.color >> 16) & 255), cg = static_cast<int>((bg.color >> 8) & 255),
              cb = static_cast<int>(bg.color & 255);

    std::vector<std::uint8_t> rowStore;
    const bool sameSize = matte.width == fg.width && matte.height == fg.height;
    if (!sameSize) rowStore.resize(static_cast<std::size_t>(fg.width));

    for (int y = 0; y < fg.height; ++y) {
        const std::uint8_t* a;
        if (sameSize) {
            a = matte.data.data() + static_cast<std::size_t>(y) * fg.width;
        } else {
            upscaleRow(matte, fg.width, fg.height, y, rowStore.data());
            a = rowStore.data();
        }
        std::uint8_t* p = fg.data.data() + static_cast<std::size_t>(y) * fg.stride;
        const std::uint8_t* q = img ? img->data.data() + static_cast<std::size_t>(y) * img->stride : nullptr;
        for (int x = 0; x < fg.width; ++x, p += 4) {
            const int al = a[x];
            if (transparent) {
                p[0] = static_cast<std::uint8_t>(div255(p[0] * al));
                p[1] = static_cast<std::uint8_t>(div255(p[1] * al));
                p[2] = static_cast<std::uint8_t>(div255(p[2] * al));
                p[3] = static_cast<std::uint8_t>(al);
                continue;
            }
            if (al < 255) {
                int bb, bgc, br;
                if (q) { bb = q[x * 4]; bgc = q[x * 4 + 1]; br = q[x * 4 + 2]; }
                else { bb = cb; bgc = cg; br = cr; }
                const int ia = 255 - al;
                p[0] = static_cast<std::uint8_t>(div255(p[0] * al + bb * ia));
                p[1] = static_cast<std::uint8_t>(div255(p[1] * al + bgc * ia));
                p[2] = static_cast<std::uint8_t>(div255(p[2] * al + br * ia));
            }
            p[3] = 255;
        }
    }
}

void resizeBilinear(const video::Frame& src, int sx, int sy, int sw, int sh, video::Frame& dst, int dw, int dh) {
    if (dw <= 0 || dh <= 0) return;
    dst.width = dw;
    dst.height = dh;
    dst.stride = dw * 4;
    dst.format = video::PixelFormat::BGRA8;
    dst.data.assign(static_cast<std::size_t>(dst.stride) * dh, 0);
    for (std::size_t i = 3; i < dst.data.size(); i += 4) dst.data[i] = 255;
    if (!frameOk(src) || sw <= 0 || sh <= 0 || sx < 0 || sy < 0 || sx + sw > src.width || sy + sh > src.height) return;

    // Per-column tables (x0, x1, weight) computed once.
    std::vector<int> x0(static_cast<std::size_t>(dw)), x1(static_cast<std::size_t>(dw)), fx(static_cast<std::size_t>(dw));
    for (int x = 0; x < dw; ++x) {
        const std::int64_t p = ((static_cast<std::int64_t>(x) * 2 + 1) * sw * 65536) / (static_cast<std::int64_t>(dw) * 2) - 32768;
        std::int64_t a = p >> 16;
        fx[static_cast<std::size_t>(x)] = static_cast<int>((p & 0xFFFF) >> 8);
        std::int64_t b = a + 1;
        a = std::clamp<std::int64_t>(a, 0, sw - 1);
        b = std::clamp<std::int64_t>(b, 0, sw - 1);
        x0[static_cast<std::size_t>(x)] = (sx + static_cast<int>(a)) * 4;
        x1[static_cast<std::size_t>(x)] = (sx + static_cast<int>(b)) * 4;
    }
    for (int y = 0; y < dh; ++y) {
        const std::int64_t p = ((static_cast<std::int64_t>(y) * 2 + 1) * sh * 65536) / (static_cast<std::int64_t>(dh) * 2) - 32768;
        std::int64_t a = p >> 16;
        const int fy = static_cast<int>((p & 0xFFFF) >> 8);
        std::int64_t b = a + 1;
        a = std::clamp<std::int64_t>(a, 0, sh - 1);
        b = std::clamp<std::int64_t>(b, 0, sh - 1);
        const std::uint8_t* r0 = src.data.data() + static_cast<std::size_t>(sy + a) * src.stride;
        const std::uint8_t* r1 = src.data.data() + static_cast<std::size_t>(sy + b) * src.stride;
        std::uint8_t* o = dst.data.data() + static_cast<std::size_t>(y) * dst.stride;
        for (int x = 0; x < dw; ++x, o += 4) {
            const int ax = x0[static_cast<std::size_t>(x)], bx = x1[static_cast<std::size_t>(x)], w = fx[static_cast<std::size_t>(x)];
            for (int c = 0; c < 3; ++c) {
                const int top = r0[ax + c] * (256 - w) + r0[bx + c] * w;
                const int bot = r1[ax + c] * (256 - w) + r1[bx + c] * w;
                o[c] = static_cast<std::uint8_t>((top * (256 - fy) + bot * fy + 32768) >> 16);
            }
            o[3] = 255;
        }
    }
}

void coverScale(const video::Frame& src, video::Frame& dst, int dw, int dh) {
    if (dw <= 0 || dh <= 0) return;
    if (!frameOk(src)) { resizeBilinear(src, 0, 0, 0, 0, dst, dw, dh); return; }
    // Largest source rectangle with the destination's aspect ratio, centred.
    std::int64_t cw = src.width, ch = src.height;
    if (cw * dh > ch * static_cast<std::int64_t>(dw)) cw = std::max<std::int64_t>(1, ch * dw / dh);   // source too wide: crop sides
    else ch = std::max<std::int64_t>(1, cw * dh / dw);                                                  // too tall: crop top/bottom
    const int cx = static_cast<int>((src.width - cw) / 2), cy = static_cast<int>((src.height - ch) / 2);
    resizeBilinear(src, cx, cy, static_cast<int>(cw), static_cast<int>(ch), dst, dw, dh);
}

namespace {
// Separable box blur on a small BGRA frame (clamped edges), alpha untouched.
void boxBlurFrame(video::Frame& f, int radius) {
    if (radius <= 0 || !frameOk(f)) return;
    radius = std::min(radius, 32);
    const int w = f.width, h = f.height, win = 2 * radius + 1;
    std::vector<std::uint8_t> tmp(f.data.size());
    for (int y = 0; y < h; ++y) {
        const std::uint8_t* r = f.data.data() + static_cast<std::size_t>(y) * f.stride;
        std::uint8_t* o = tmp.data() + static_cast<std::size_t>(y) * f.stride;
        for (int c = 0; c < 3; ++c) {
            int sum = 0;
            for (int k = -radius; k <= radius; ++k) sum += r[std::clamp(k, 0, w - 1) * 4 + c];
            for (int x = 0; x < w; ++x) {
                o[x * 4 + c] = static_cast<std::uint8_t>((sum + win / 2) / win);
                sum += r[std::min(x + radius + 1, w - 1) * 4 + c] - r[std::max(x - radius, 0) * 4 + c];
            }
        }
    }
    for (int x = 0; x < w; ++x) {
        for (int c = 0; c < 3; ++c) {
            int sum = 0;
            for (int k = -radius; k <= radius; ++k) sum += tmp[static_cast<std::size_t>(std::clamp(k, 0, h - 1)) * f.stride + x * 4 + c];
            for (int y = 0; y < h; ++y) {
                f.data[static_cast<std::size_t>(y) * f.stride + x * 4 + c] = static_cast<std::uint8_t>((sum + win / 2) / win);
                sum += tmp[static_cast<std::size_t>(std::min(y + radius + 1, h - 1)) * f.stride + x * 4 + c] -
                       tmp[static_cast<std::size_t>(std::max(y - radius, 0)) * f.stride + x * 4 + c];
            }
        }
    }
}
}  // namespace

void makeBlurBackground(const video::Frame& src, int strength, video::Frame& out) {
    if (!frameOk(src)) {
        resizeBilinear(src, 0, 0, 0, 0, out, std::max(src.width, 1), std::max(src.height, 1));
        return;
    }
    strength = std::clamp(strength, 0, 100);
    if (strength == 0) {
        out = src;
        for (std::size_t i = 3; i < out.data.size(); i += 4) out.data[i] = 255;
        return;
    }
    const int sw = std::max(8, src.width / 8), sh = std::max(8, src.height / 8);
    video::Frame small;
    video::downscaleBox(src, small, sw, sh);
    boxBlurFrame(small, 1 + strength / 14);   // 1..8 px at 1/8 resolution (two cascaded passes below)
    boxBlurFrame(small, 1 + strength / 14);
    resizeBilinear(small, 0, 0, small.width, small.height, out, src.width, src.height);
    out.ptsNs = src.ptsNs;
    out.index = src.index;
}

}  // namespace hb::chroma
