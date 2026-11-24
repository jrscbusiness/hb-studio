#pragma once
// Windows text backend for overlays: plain GDI (CreateFontW + TextOutW into a 32-bit DIB). GDI was chosen
// over DirectWrite/GDI+ because it needs no COM initialisation, no extra headers/libs beyond gdi32 and has
// the smallest API surface (the least risk of a compile error that cannot be checked off-Windows).
// Only a white-on-black coverage mask is taken from GDI; colours, outline and shadow are applied by the
// portable MaskTextRasterizer. Emoji: GDI font linking draws monochrome fallback glyphs when the
// selected font lacks them (colour emoji are not supported). NOT VALIDATED on real hardware yet.
#include <memory>

#include "overlay/text.hpp"

namespace hb::overlay {

std::shared_ptr<IGlyphSource> makeGdiGlyphSource();
std::shared_ptr<ITextRasterizer> makeGdiTextRasterizer();

}  // namespace hb::overlay
