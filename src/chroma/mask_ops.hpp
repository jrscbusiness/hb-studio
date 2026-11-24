#pragma once
// Matte post-processing, all portable and allocation-light. Masks are 8-bit (255 = person).
#include "chroma/types.hpp"
#include "video/program_output.hpp"

namespace hb::chroma {

// out = smoothstep((v - lo) / (hi - lo)) applied per pixel; lo/hi in 0..255 (lo < hi enforced).
// Turns a soft probability map into a crisper matte with a controllable transition band.
void applyLevels(Mask& m, int lo, int hi);

// 3x3 min filter / max filter, `passes` times. Handles any size, including 1xN.
void erode(Mask& m, int passes);
void dilate(Mask& m, int passes);

// Separable box blur of radius r (r<=0: no-op). Edges clamp.
void boxBlur(Mask& m, int radius);

// Temporal smoothing: acc = (acc*(256-wNew) + incoming*wNew) / 256. If acc is invalid or its
// size differs, it is replaced by `incoming`. wNew in 1..256 (256 = no smoothing).
void emaBlend(Mask& acc, const Mask& incoming, int wNew);

// Bilinear sample of `m` at (x+0.5)/w, (y+0.5)/h of a frame of size w x h, written into `out`.
void upscaleBilinear(const Mask& m, Mask& out, int w, int h);

// Reads one output row of the bilinear upscale of `m` to w x h into row[0..w). Used by the
// compositor so no full-frame mask is ever materialised.
void upscaleRow(const Mask& m, int w, int h, int y, std::uint8_t* row);

// Builds a mask from the alpha-ish probability buffer (float 0..1) with clamping and NaN guard.
std::uint8_t probToByte(float p);

}  // namespace hb::chroma
