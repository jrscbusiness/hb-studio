#pragma once
// Compositing of a keyed foreground over a replacement background, plus the helpers that build
// the replacement (blurred copy of the frame, cover-scaled image).
//
// ALPHA CONVENTION (documented in docs/CHROMA.md, relied on by overlay/scene code):
//   Frames are BGRA8 with PREMULTIPLIED alpha: B,G,R are already multiplied by A/255.
//   * Opaque replacement (colour/blur/image): every output pixel has A = 255.
//   * ReplKind::Transparent: A = matte, colour channels premultiplied (B*A/255 ...). Blending such
//     a frame "over" another is  out = fg + bg * (255 - A) / 255  per channel (no multiply by A).
//   Consumers that only know opaque BGRA (JPEG preview, MF encoders) simply show transparent areas
//   as black, which is the correct premultiplied-over-black result.
#include "chroma/chroma_key.hpp"
#include "chroma/mask_ops.hpp"
#include "chroma/types.hpp"
#include "video/program_output.hpp"

namespace hb::chroma {

struct Background {
    ReplKind kind = ReplKind::Color;
    std::uint32_t color = 0;                 // 0xRRGGBB for Color
    const video::Frame* image = nullptr;     // same size as the target for Blur/Image; null -> falls back to `color`
};

// Composites `fg` (modified in place) with matte `matte` (any size: bilinear-sampled to fg's size)
// over `bg`. `spill`/`spillAmount` run the despill on the foreground first.
void compositeMatte(video::Frame& fg, const Mask& matte, const Background& bg, Spill spill = Spill::None,
                    int spillAmount = 0);

// Fixed-point bilinear resize of the source rectangle (sx,sy,sw,sh) of `src` into `dst` (dw x dh,
// allocated here). Any degenerate input leaves `dst` black/opaque rather than reading out of bounds.
void resizeBilinear(const video::Frame& src, int sx, int sy, int sw, int sh, video::Frame& dst, int dw, int dh);

// "Cover" scaling: fills dw x dh keeping aspect ratio, cropping the overflow (centre crop).
void coverScale(const video::Frame& src, video::Frame& dst, int dw, int dh);

// Blurred, opaque copy of `src` at the same size. strength 0..100 (0 = a plain copy). Works at
// ~1/8 resolution so its cost is dominated by the box-downscale and the final upscale.
void makeBlurBackground(const video::Frame& src, int strength, video::Frame& out);

}  // namespace hb::chroma
