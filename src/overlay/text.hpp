#pragma once
// Text rasterization for overlays.
//
//  ITextRasterizer   what the layers use: measure + rasterize UTF-8 text (word-wrapped, with outline and
//                    shadow) into a straight-alpha BGRA Bitmap.
//  IGlyphSource      the small platform backend: measures a single line and renders its 8-bit coverage
//                    mask. The Windows implementation (GDI) is in overlay/windows/gdi_glyphs.*; the
//                    built-in bitmap font below is a deterministic fake for tests and non-Windows builds.
//  MaskTextRasterizer implements ITextRasterizer on top of any IGlyphSource: word wrap, alignment,
//                    outline (dilation) and shadow are portable and unit-tested.
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "overlay/blend.hpp"

namespace hb::overlay {

enum class Align { Left, Center, Right };

struct TextStyle {
    std::string font = "Segoe UI";
    int px = 32;                    // glyph cell height in pixels
    bool bold = false;
    std::uint32_t color = 0xFFFFFF;  // 0xRRGGBB
    int outlinePx = 0;              // 0 = none
    std::uint32_t outlineColor = 0x000000;
    int shadowPx = 0;               // offset down/right; 0 = none
    std::uint32_t shadowColor = 0x000000;
    int shadowAlpha = 160;          // 0..255
    Align align = Align::Left;
    int maxLines = 0;               // 0 = unlimited; extra lines are dropped
    bool keepTail = false;          // with maxLines: keep the LAST lines (captions) instead of the first
    int lineSpacingPct = 100;       // line pitch as % of the font's line height

    // Stable string used as a cache key (every field that changes the pixels).
    std::string key() const;
};

struct TextMetrics {
    int width = 0, height = 0, lines = 0;
};

class ITextRasterizer {
public:
    virtual ~ITextRasterizer() = default;
    // Size the rasterized text would have (including outline/shadow margins). maxWidth <= 0: no wrapping.
    virtual bool measure(const std::string& utf8, const TextStyle& style, int maxWidth, TextMetrics& out) = 0;
    // Renders into `out` (straight alpha, transparent background). Returns false for empty text / failure.
    virtual bool rasterize(const std::string& utf8, const TextStyle& style, int maxWidth, Bitmap& out) = 0;
};

class IGlyphSource {
public:
    virtual ~IGlyphSource() = default;
    // Pixel width of one line (no '\n') and the line height for the style.
    virtual int advance(const std::string& line, const TextStyle& style) = 0;
    virtual int lineHeight(const TextStyle& style) = 0;
    // Renders one line as an 8-bit coverage mask of size advance() x lineHeight().
    virtual bool drawMask(const std::string& line, const TextStyle& style, std::vector<std::uint8_t>& mask, int& w, int& h) = 0;
};

class MaskTextRasterizer : public ITextRasterizer {
public:
    explicit MaskTextRasterizer(std::shared_ptr<IGlyphSource> glyphs) : glyphs_(std::move(glyphs)) {}
    bool measure(const std::string& utf8, const TextStyle& style, int maxWidth, TextMetrics& out) override;
    bool rasterize(const std::string& utf8, const TextStyle& style, int maxWidth, Bitmap& out) override;

    // Word wrap exposed for tests: hard '\n' breaks, breaks at spaces, over-long words split by code point.
    std::vector<std::string> wrap(const std::string& utf8, const TextStyle& style, int maxWidth);

private:
    std::shared_ptr<IGlyphSource> glyphs_;
};

// ---- UTF-8 helpers ----
// Number of bytes of the UTF-8 sequence starting at s[i] (1 for invalid bytes, so progress is guaranteed).
std::size_t utf8SeqLen(std::string_view s, std::size_t i);
// Decodes the code point at s[i]; invalid sequences yield U+FFFD.
std::uint32_t utf8Decode(std::string_view s, std::size_t i);
// Removes control characters (except '\n'), bounds the length and repairs invalid UTF-8.
std::string sanitizeUtf8(std::string_view s, std::size_t maxBytes);

// Deterministic built-in 5x7 bitmap font (digits, A-Z (lowercase is drawn as uppercase), common
// punctuation; any other code point gets a boxed placeholder glyph). For tests and non-Windows builds.
std::shared_ptr<IGlyphSource> makeBuiltinGlyphSource();
std::shared_ptr<ITextRasterizer> makeBuiltinTextRasterizer();

}  // namespace hb::overlay
