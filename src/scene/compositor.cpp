#include "scene/compositor.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>

#include "video/frame_ops.hpp"

namespace hb::scene {

namespace {

constexpr int kMaxDim = 8192;
constexpr std::int64_t kMaxMaskArea = 64LL * 1024 * 1024;

inline int div255(int v) { return ((v + 128) * 257) >> 16; }  // exact for 0..65025
inline std::uint8_t u8(int v) { return static_cast<std::uint8_t>(std::clamp(v, 0, 255)); }

void fillSolid(video::Frame& f, const Color& c) {
    for (std::size_t i = 0; i + 3 < f.data.size(); i += 4) {
        f.data[i] = c.b;
        f.data[i + 1] = c.g;
        f.data[i + 2] = c.r;
        f.data[i + 3] = 255;
    }
}

void makeFrame(video::Frame& f, int w, int h) {
    f.width = w;
    f.height = h;
    f.stride = w * 4;
    f.format = video::PixelFormat::BGRA8;
    f.data.assign(static_cast<std::size_t>(f.stride) * static_cast<std::size_t>(h), 0);
}

void fillGradient(video::Frame& f, const Color& a, const Color& b, double angleDeg) {
    const double rad = angleDeg * 3.14159265358979323846 / 180.0;
    const double cs = std::cos(rad), sn = std::sin(rad);
    const double W = f.width, H = f.height;
    const double len = std::max(1e-6, std::fabs(W * cs) + std::fabs(H * sn));
    for (int y = 0; y < f.height; ++y) {
        std::uint8_t* row = f.data.data() + static_cast<std::size_t>(y) * f.stride;
        for (int x = 0; x < f.width; ++x) {
            const double t = std::clamp(((x + 0.5 - W / 2) * cs + (y + 0.5 - H / 2) * sn) / len + 0.5, 0.0, 1.0);
            row[x * 4 + 0] = u8(static_cast<int>(a.b + (b.b - a.b) * t + 0.5));
            row[x * 4 + 1] = u8(static_cast<int>(a.g + (b.g - a.g) * t + 0.5));
            row[x * 4 + 2] = u8(static_cast<int>(a.r + (b.r - a.r) * t + 0.5));
            row[x * 4 + 3] = 255;
        }
    }
}

// Cover-scales `img` (nearest) over the whole frame.
void fillImage(video::Frame& f, const video::Frame& img) {
    const double s = std::max(static_cast<double>(f.width) / img.width, static_cast<double>(f.height) / img.height);
    const double cw = img.width * s, ch = img.height * s;
    const double ox = (cw - f.width) / 2, oy = (ch - f.height) / 2;
    for (int y = 0; y < f.height; ++y) {
        const int sy = std::clamp(static_cast<int>((y + oy) / s), 0, img.height - 1);
        const std::uint8_t* srow = img.data.data() + static_cast<std::size_t>(sy) * img.stride;
        std::uint8_t* drow = f.data.data() + static_cast<std::size_t>(y) * f.stride;
        for (int x = 0; x < f.width; ++x) {
            const int sx = std::clamp(static_cast<int>((x + ox) / s), 0, img.width - 1);
            std::memcpy(drow + x * 4, srow + sx * 4, 3);
            drow[x * 4 + 3] = 255;
        }
    }
}

// Signed distance (pixels, negative inside) from the centre-relative point to a rounded rect.
inline double sdfRounded(double px, double py, double hw, double hh, double r) {
    const double qx = std::fabs(px) - (hw - r), qy = std::fabs(py) - (hh - r);
    const double ox = std::max(qx, 0.0), oy = std::max(qy, 0.0);
    return std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.0) - r;
}
inline double cover(double d) { return std::clamp(0.5 - d, 0.0, 1.0); }

void boxBlurH(std::vector<std::uint8_t>& buf, int w, int h, int r) {
    if (r <= 0) return;
    std::vector<std::uint8_t> row(static_cast<std::size_t>(w));
    const int div = 2 * r + 1;
    for (int y = 0; y < h; ++y) {
        std::uint8_t* p = buf.data() + static_cast<std::size_t>(y) * w;
        int sum = 0;
        for (int x = 0; x <= r && x < w; ++x) sum += p[x];
        for (int x = 0; x < w; ++x) {
            row[static_cast<std::size_t>(x)] = static_cast<std::uint8_t>((sum + div / 2) / div);
            const int add = x + r + 1, sub = x - r;
            if (add < w) sum += p[add];
            if (sub >= 0) sum -= p[sub];
        }
        std::memcpy(p, row.data(), static_cast<std::size_t>(w));
    }
}

void boxBlurV(std::vector<std::uint8_t>& buf, int w, int h, int r) {
    if (r <= 0) return;
    const std::vector<std::uint8_t> src = buf;
    std::vector<int> sum(static_cast<std::size_t>(w), 0);
    const int div = 2 * r + 1;
    for (int y = 0; y <= r && y < h; ++y)
        for (int x = 0; x < w; ++x) sum[static_cast<std::size_t>(x)] += src[static_cast<std::size_t>(y) * w + x];
    for (int y = 0; y < h; ++y) {
        std::uint8_t* o = buf.data() + static_cast<std::size_t>(y) * w;
        for (int x = 0; x < w; ++x) o[x] = static_cast<std::uint8_t>((sum[static_cast<std::size_t>(x)] + div / 2) / div);
        const int add = y + r + 1, sub = y - r;
        if (add < h)
            for (int x = 0; x < w; ++x) sum[static_cast<std::size_t>(x)] += src[static_cast<std::size_t>(add) * w + x];
        if (sub >= 0)
            for (int x = 0; x < w; ++x) sum[static_cast<std::size_t>(x)] -= src[static_cast<std::size_t>(sub) * w + x];
    }
}

inline void blendPx(std::uint8_t* d, std::uint8_t b, std::uint8_t g, std::uint8_t r, int a) {
    if (a >= 255) { d[0] = b; d[1] = g; d[2] = r; d[3] = 255; return; }
    if (a <= 0) return;
    const int ia = 255 - a;
    d[0] = static_cast<std::uint8_t>(div255(b * a + d[0] * ia));
    d[1] = static_cast<std::uint8_t>(div255(g * a + d[1] * ia));
    d[2] = static_cast<std::uint8_t>(div255(r * a + d[2] * ia));
    d[3] = 255;
}

}  // namespace

// ------------------------------------------------------------------------------------ BMP

bool decodeBmp(const std::uint8_t* d, std::size_t n, video::Frame& out) {
    auto rd32 = [&](std::size_t o) -> std::int64_t {
        return static_cast<std::int32_t>(static_cast<std::uint32_t>(d[o]) | (static_cast<std::uint32_t>(d[o + 1]) << 8) |
                                         (static_cast<std::uint32_t>(d[o + 2]) << 16) | (static_cast<std::uint32_t>(d[o + 3]) << 24));
    };
    auto rd16 = [&](std::size_t o) -> int { return d[o] | (d[o + 1] << 8); };
    if (!d || n < 54 || d[0] != 'B' || d[1] != 'M') return false;
    const std::int64_t off = static_cast<std::uint32_t>(rd32(10));
    const std::int64_t hdr = rd32(14);
    if (hdr < 40) return false;
    const std::int64_t w = rd32(18);
    std::int64_t h = rd32(22);
    const int bpp = rd16(28), comp = static_cast<int>(rd32(30));
    if (rd16(26) != 1 || (bpp != 24 && bpp != 32) || (comp != 0 && !(comp == 3 && bpp == 32))) return false;
    const bool topDown = h < 0;
    if (topDown) h = -h;
    if (w <= 0 || h <= 0 || w > kMaxDim || h > kMaxDim) return false;
    const std::int64_t rowBytes = ((w * bpp + 31) / 32) * 4;
    if (off < 0 || off + rowBytes * h > static_cast<std::int64_t>(n)) return false;
    makeFrame(out, static_cast<int>(w), static_cast<int>(h));
    const int bytesPx = bpp / 8;
    for (std::int64_t y = 0; y < h; ++y) {
        const std::int64_t sy = topDown ? y : h - 1 - y;
        const std::uint8_t* s = d + off + sy * rowBytes;
        std::uint8_t* o = out.data.data() + static_cast<std::size_t>(y) * out.stride;
        for (std::int64_t x = 0; x < w; ++x) {
            o[x * 4 + 0] = s[x * bytesPx + 0];
            o[x * 4 + 1] = s[x * bytesPx + 1];
            o[x * 4 + 2] = s[x * bytesPx + 2];
            o[x * 4 + 3] = 255;
        }
    }
    return true;
}

bool loadBmp(const std::string& path, video::Frame& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string s = ss.str();
    if (s.size() > 96u * 1024 * 1024) return false;
    return decodeBmp(reinterpret_cast<const std::uint8_t*>(s.data()), s.size(), out);
}

// -------------------------------------------------------------------------------- compile

Color placeholderColor(const std::string& role, int slot) {
    std::uint32_t h = 2166136261u;
    for (unsigned char c : role) { h ^= c; h *= 16777619u; }
    if (role.empty()) h = static_cast<std::uint32_t>(slot + 7) * 2654435761u;
    const double hue = (h % 360) / 60.0;
    const double s = 0.45, v = 0.62;
    const double c = v * s, x = c * (1 - std::fabs(std::fmod(hue, 2.0) - 1)), m = v - c;
    double r = 0, g = 0, b = 0;
    const int seg = static_cast<int>(hue);
    switch (seg) {
        case 0: r = c; g = x; break;
        case 1: r = x; g = c; break;
        case 2: g = c; b = x; break;
        case 3: g = x; b = c; break;
        case 4: r = x; b = c; break;
        default: r = c; b = x; break;
    }
    Color o;
    o.r = u8(static_cast<int>((r + m) * 255));
    o.g = u8(static_cast<int>((g + m) * 255));
    o.b = u8(static_cast<int>((b + m) * 255));
    return o;
}

std::shared_ptr<const CompiledScene> compileScene(const Scene& scene, int W, int H, const std::vector<double>& aspects,
                                                  const ImageLoader& loader) {
    auto cs = std::make_shared<CompiledScene>();
    cs->width = W;
    cs->height = H;
    cs->aspects.assign(scene.layers.size(), 16.0 / 9.0);
    for (std::size_t i = 0; i < scene.layers.size() && i < aspects.size(); ++i)
        if (aspects[i] > 0.05 && aspects[i] < 20) cs->aspects[i] = aspects[i];
    if (W <= 0 || H <= 0 || W > kMaxDim || H > kMaxDim) {
        cs->width = cs->height = 0;
        return cs;
    }

    // ---- background
    makeFrame(cs->bg, W, H);
    bool done = false;
    switch (scene.bg.type) {
        case Background::Type::Solid: fillSolid(cs->bg, scene.bg.from); done = true; break;
        case Background::Type::Gradient: fillGradient(cs->bg, scene.bg.from, scene.bg.to, scene.bg.angle); done = true; break;
        case Background::Type::Image: {
            video::Frame img;
            const bool ok = !scene.bg.imagePath.empty() &&
                            (loader ? loader(scene.bg.imagePath, img) : loadBmp(scene.bg.imagePath, img)) && img.width > 0 &&
                            img.height > 0 &&
                            img.data.size() >= static_cast<std::size_t>(img.stride) * static_cast<std::size_t>(img.height);
            if (ok) { fillImage(cs->bg, img); done = true; }
            break;
        }
    }
    if (!done) fillSolid(cs->bg, scene.bg.from);

    // ---- layers, bottom -> top
    std::vector<int> order(scene.layers.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = static_cast<int>(i);
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return scene.layers[static_cast<std::size_t>(a)].z < scene.layers[static_cast<std::size_t>(b)].z; });

    bool first = true;
    for (int li : order) {
        const Layer& L = scene.layers[static_cast<std::size_t>(li)];
        if (L.opacity <= 0) continue;
        const double a = cs->aspects[static_cast<std::size_t>(li)];
        const int boxX = static_cast<int>(std::floor(L.rect.x * W + 0.5));
        const int boxY = static_cast<int>(std::floor(L.rect.y * H + 0.5));
        const int boxW = std::max(1, static_cast<int>(std::floor(L.rect.w * W + 0.5)));
        const int boxH = std::max(1, static_cast<int>(std::floor(L.rect.h * H + 0.5)));

        CompiledLayer cl;
        cl.layer = li;
        cl.slot = L.slot;
        cl.opacity256 = static_cast<int>(std::lround(std::clamp(L.opacity, 0.0, 1.0) * 256));
        if (L.fit == FitMode::Fit) {
            int cw, ch;
            if (boxW >= boxH * a) { ch = boxH; cw = static_cast<int>(std::lround(ch * a)); }
            else { cw = boxW; ch = static_cast<int>(std::lround(cw / a)); }
            cw = std::clamp(cw, 1, boxW);
            ch = std::clamp(ch, 1, boxH);
            cl.dx = boxX + (boxW - cw) / 2;
            cl.dy = boxY + (boxH - ch) / 2;
            cl.dw = cw;
            cl.dh = ch;
            cl.srcW = cw;
            cl.srcH = ch;
        } else {
            const double zoom = L.fit == FitMode::Crop ? std::clamp(L.zoom, 1.0, 4.0) : 1.0;
            double sw, sh;
            if (boxW >= boxH * a) { sw = boxW; sh = boxW / a; } else { sh = boxH; sw = boxH * a; }
            cl.srcW = std::clamp(static_cast<int>(std::lround(sw * zoom)), boxW, kMaxDim);
            cl.srcH = std::clamp(static_cast<int>(std::lround(sh * zoom)), boxH, kMaxDim);
            cl.dx = boxX;
            cl.dy = boxY;
            cl.dw = boxW;
            cl.dh = boxH;
            const double ax = L.fit == FitMode::Crop ? L.cropX : 0.5, ay = L.fit == FitMode::Crop ? L.cropY : 0.5;
            cl.offX = std::clamp(static_cast<int>(std::lround((cl.srcW - boxW) * ax)), 0, cl.srcW - boxW);
            cl.offY = std::clamp(static_cast<int>(std::lround((cl.srcH - boxH) * ay)), 0, cl.srcH - boxH);
        }
        if (static_cast<std::int64_t>(cl.dw) * cl.dh > kMaxMaskArea || static_cast<std::int64_t>(cl.srcW) * cl.srcH > kMaxMaskArea) continue;
        if (cl.dx >= W || cl.dy >= H || cl.dx + cl.dw <= 0 || cl.dy + cl.dh <= 0) continue;  // fully off-screen

        const bool covers = cl.dx == 0 && cl.dy == 0 && cl.dw == W && cl.dh == H;
        const double minSide = std::min(cl.dw, cl.dh);
        const double radius = std::min(L.radius * minSide, minSide / 2.0);
        const double hw = cl.dw / 2.0, hh = cl.dh / 2.0;
        const std::size_t area = static_cast<std::size_t>(cl.dw) * static_cast<std::size_t>(cl.dh);

        cl.direct = first && covers && cl.srcW == W && cl.srcH == H && cl.offX == 0 && cl.offY == 0 && radius < 0.5 &&
                    L.border.width <= 0 && cl.opacity256 >= 256;
        first = false;
        if (!cl.direct) {
            if (radius >= 0.5) {
                cl.mask.assign(area, 255);
                const double cx0 = hw - radius, cy0 = hh - radius;
                for (int y = 0; y < cl.dh; ++y) {
                    const double py = y + 0.5 - hh;
                    for (int x = 0; x < cl.dw; ++x) {
                        const double px = x + 0.5 - hw;
                        if (std::fabs(px) <= cx0 || std::fabs(py) <= cy0) continue;  // straight edge / interior: full coverage
                        cl.mask[static_cast<std::size_t>(y) * cl.dw + x] = u8(static_cast<int>(cover(sdfRounded(px, py, hw, hh, radius)) * 255 + 0.5));
                    }
                }
            }
            if (L.border.width > 0 && L.border.color.a > 0) {
                const double bw = std::max(1.0, L.border.width * H);
                cl.ring.assign(area, 0);
                cl.ringColor = L.border.color;
                for (int y = 0; y < cl.dh; ++y) {
                    const double py = y + 0.5 - hh;
                    for (int x = 0; x < cl.dw; ++x) {
                        const double px = x + 0.5 - hw;
                        const double d = sdfRounded(px, py, hw, hh, radius);
                        const double c = cover(d) * (1.0 - cover(d + bw));
                        cl.ring[static_cast<std::size_t>(y) * cl.dw + x] = u8(static_cast<int>(c * 255 + 0.5));
                    }
                }
            }
            if (L.shadow.enabled() && !covers) {
                const int r = std::max(1, static_cast<int>(std::lround(L.shadow.blur * H * 0.5)));
                const int m = 2 * r + 2;
                const std::int64_t bw = cl.dw + 2LL * m, bh = cl.dh + 2LL * m;
                if (bw * bh <= 16LL * 1024 * 1024) {
                    std::vector<std::uint8_t> buf(static_cast<std::size_t>(bw * bh), 0);
                    for (int y = 0; y < cl.dh; ++y) {
                        std::uint8_t* row = buf.data() + static_cast<std::size_t>(y + m) * bw + m;
                        if (cl.mask.empty()) std::memset(row, 255, static_cast<std::size_t>(cl.dw));
                        else std::memcpy(row, cl.mask.data() + static_cast<std::size_t>(y) * cl.dw, static_cast<std::size_t>(cl.dw));
                    }
                    for (int pass = 0; pass < 2; ++pass) {
                        boxBlurH(buf, static_cast<int>(bw), static_cast<int>(bh), r);
                        boxBlurV(buf, static_cast<int>(bw), static_cast<int>(bh), r);
                    }
                    const int op = static_cast<int>(std::lround(std::clamp(L.shadow.opacity, 0.0, 1.0) * 256 * (cl.opacity256 / 256.0)));
                    for (auto& v : buf) v = u8((v * op) >> 8);
                    cl.shadow = std::move(buf);
                    cl.shW = static_cast<int>(bw);
                    cl.shH = static_cast<int>(bh);
                    cl.shX = cl.dx + static_cast<int>(std::lround(L.shadow.dx * H)) - m;
                    cl.shY = cl.dy + static_cast<int>(std::lround(L.shadow.dy * H)) - m;
                }
            }
        }
        cs->layers.push_back(std::move(cl));
    }
    return cs;
}

// --------------------------------------------------------------------------------- compose

void composeScene(const CompiledScene& cs, const Scene& scene, const LayerRenderFn& render, video::Frame& out,
                  std::uint64_t index) {
    if (out.width <= 0 || out.height <= 0 ||
        out.data.size() < static_cast<std::size_t>(out.stride) * static_cast<std::size_t>(out.height))
        return;
    if (cs.width != out.width || cs.height != out.height || out.stride != out.width * 4 ||
        cs.bg.data.size() != static_cast<std::size_t>(cs.width) * cs.height * 4) {
        // Compiled for another size: never read/write out of bounds.
        for (std::size_t i = 0; i + 3 < out.data.size(); i += 4) { out.data[i] = out.data[i + 1] = out.data[i + 2] = 0; out.data[i + 3] = 255; }
        return;
    }
    const int W = out.width, H = out.height;
    const std::size_t bytes = static_cast<std::size_t>(W) * H * 4;
    bool haveBg = false;
    thread_local std::vector<video::Frame> scratch;  // per rendering thread: no allocation in steady state

    for (std::size_t j = 0; j < cs.layers.size(); ++j) {
        const CompiledLayer& cl = cs.layers[j];
        if (cl.layer < 0 || static_cast<std::size_t>(cl.layer) >= scene.layers.size()) continue;
        const Layer& L = scene.layers[static_cast<std::size_t>(cl.layer)];

        if (cl.direct) {
            bool ok = false;
            try { ok = render(L, out, index); } catch (...) { ok = false; }
            if (!ok) fillSolid(out, placeholderColor(L.role, L.slot));
            // A source with transparency (e.g. a chroma-keyed camera) shows the background through.
            std::uint8_t* p = out.data.data();
            const std::uint8_t* b = cs.bg.data.data();
            for (std::size_t i = 0; i < bytes; i += 4) {
                if (p[i + 3] != 255) {
                    const int a = p[i + 3];
                    std::uint8_t px[4] = {b[i], b[i + 1], b[i + 2], 255};
                    blendPx(px, p[i], p[i + 1], p[i + 2], a);
                    std::memcpy(p + i, px, 4);
                }
            }
            haveBg = true;
            continue;
        }
        if (!haveBg) {
            std::memcpy(out.data.data(), cs.bg.data.data(), bytes);
            haveBg = true;
        }

        // ---- drop shadow (black, multiplies what is below)
        if (!cl.shadow.empty()) {
            const int y0 = std::max(0, cl.shY), y1 = std::min(H, cl.shY + cl.shH);
            const int x0 = std::max(0, cl.shX), x1 = std::min(W, cl.shX + cl.shW);
            for (int y = y0; y < y1; ++y) {
                const std::uint8_t* s = cl.shadow.data() + static_cast<std::size_t>(y - cl.shY) * cl.shW + (x0 - cl.shX);
                std::uint8_t* d = out.data.data() + static_cast<std::size_t>(y) * out.stride + static_cast<std::size_t>(x0) * 4;
                for (int x = x0; x < x1; ++x, ++s, d += 4) {
                    if (!*s) continue;
                    const int ia = 255 - *s;
                    d[0] = static_cast<std::uint8_t>(div255(d[0] * ia));
                    d[1] = static_cast<std::uint8_t>(div255(d[1] * ia));
                    d[2] = static_cast<std::uint8_t>(div255(d[2] * ia));
                }
            }
        }

        // ---- source into the per-thread scratch, then masked blend
        if (scratch.size() <= j) scratch.resize(j + 1);
        video::Frame& sf = scratch[j];
        sf.width = cl.srcW;
        sf.height = cl.srcH;
        sf.stride = cl.srcW * 4;
        sf.format = video::PixelFormat::BGRA8;
        const std::size_t need = static_cast<std::size_t>(sf.stride) * static_cast<std::size_t>(cl.srcH);
        if (sf.data.size() != need) sf.data.resize(need);
        bool ok = false;
        try { ok = render(L, sf, index); } catch (...) { ok = false; }
        if (!ok) fillSolid(sf, placeholderColor(L.role, L.slot));

        const int y0 = std::max(0, cl.dy), y1 = std::min(H, cl.dy + cl.dh);
        const int x0 = std::max(0, cl.dx), x1 = std::min(W, cl.dx + cl.dw);
        const int op = cl.opacity256;
        for (int y = y0; y < y1; ++y) {
            const int ly = y - cl.dy;
            const std::uint8_t* s = sf.data.data() + static_cast<std::size_t>(ly + cl.offY) * sf.stride +
                                    static_cast<std::size_t>(x0 - cl.dx + cl.offX) * 4;
            std::uint8_t* d = out.data.data() + static_cast<std::size_t>(y) * out.stride + static_cast<std::size_t>(x0) * 4;
            const std::uint8_t* m = cl.mask.empty() ? nullptr : cl.mask.data() + static_cast<std::size_t>(ly) * cl.dw + (x0 - cl.dx);
            for (int x = x0; x < x1; ++x, s += 4, d += 4) {
                int a = s[3];
                if (m) a = div255(a * *m++);
                if (op != 256) a = (a * op) >> 8;
                blendPx(d, s[0], s[1], s[2], a);
            }
        }

        // ---- border ring
        if (!cl.ring.empty()) {
            const int ca = cl.ringColor.a;
            for (int y = y0; y < y1; ++y) {
                const int ly = y - cl.dy;
                const std::uint8_t* r = cl.ring.data() + static_cast<std::size_t>(ly) * cl.dw + (x0 - cl.dx);
                std::uint8_t* d = out.data.data() + static_cast<std::size_t>(y) * out.stride + static_cast<std::size_t>(x0) * 4;
                for (int x = x0; x < x1; ++x, ++r, d += 4) {
                    if (!*r) continue;
                    int a = div255(*r * ca);
                    if (op != 256) a = (a * op) >> 8;
                    blendPx(d, cl.ringColor.b, cl.ringColor.g, cl.ringColor.r, a);
                }
            }
        }
    }
    if (!haveBg) std::memcpy(out.data.data(), cs.bg.data.data(), bytes);
}

void renderThumbnail(const Scene& scene, int width, int height, video::Frame& out) {
    video::prepareFrame(out, width, height);
    auto cs = compileScene(scene, width, height, {});
    composeScene(*cs, scene, [](const Layer&, video::Frame&, std::uint64_t) { return false; }, out, 0);
}

}  // namespace hb::scene
