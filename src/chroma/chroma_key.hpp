#pragma once
// Classic green/blue-screen chroma key (the cheap fallback, no model needed).
//
// Distance is measured in the CbCr plane of YCbCr (BT.601, full range) and IGNORES luma, so a
// shadow on the screen still keys out. alpha = smoothstep over [d0, d0 + band] where
// d0 = tolerance * 0.9 and band = softness * 0.9 + 1 (CbCr units, 0..~90).
#include "chroma/types.hpp"
#include "video/program_output.hpp"

namespace hb::chroma {

struct KeyParams {
    Mode mode = Mode::Green;   // Green or Blue (anything else is treated as Green)
    int tolerance = 45;        // 0..100
    int softness = 25;         // 0..100
    // Override of the reference key colour in CbCr (-1 = use the built-in typical green/blue screen).
    int keyCb = -1, keyCr = -1;
};

// Reference CbCr of a typical (not pure!) green / blue screen: pure (0,255,0) is NOT what real
// screens look like once lit and run through a webcam.
void referenceKeyCbCr(Mode m, int& cb, int& cr);

// Writes a matte the size of `f` (255 = keep, 0 = screen). Safe on degenerate/short frames.
void computeKeyMask(const video::Frame& f, const KeyParams& p, Mask& out);

enum class Spill { None, Green, Blue };
// Removes the coloured fringe the screen reflects onto the subject: G (or B) is clamped to
// max of the other two channels, blended by `amount` 0..100. Operates in place on BGRA pixels.
void despill(video::Frame& f, Spill kind, int amount);

}  // namespace hb::chroma
