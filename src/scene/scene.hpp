#pragma once
// Scene model: background + ordered source layers + optional overlay presets (plain data).
// Portable (no OS headers). JSON (de)serialisation is strict, bounded and never throws.
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "core/json.hpp"

namespace hb::scene {

struct Color {
    std::uint8_t r = 0, g = 0, b = 0, a = 255;
    bool operator==(const Color& o) const { return r == o.r && g == o.g && b == o.b && a == o.a; }
};

// "#rgb", "#rrggbb" or "#rrggbbaa". Returns false (out untouched) on anything else.
bool parseColor(const std::string& text, Color& out);
std::string colorToString(const Color& c);

// Normalised rectangle: 0..1 spans the Program frame (values slightly outside are allowed so
// a layer can bleed off an edge). Aspect: a rect with w == h is a 16:9 box on a 16:9 frame.
struct Rect {
    double x = 0, y = 0, w = 1, h = 1;
};

enum class FitMode {
    Fit,   // whole source visible, letterboxed (the visible rect shrinks to the content)
    Fill,  // covers the box, cropping the overflow (centered)
    Crop,  // like Fill but with zoom >= 1 and an anchor (cropX/cropY, 0..1)
};
const char* fitName(FitMode m);
bool parseFit(const std::string& s, FitMode& out);

struct Border {
    double width = 0;  // fraction of the frame height (0 = none)
    Color color{255, 255, 255, 255};
};

struct Shadow {
    double blur = 0;  // fraction of the frame height (0 = no shadow)
    double dx = 0, dy = 0;
    double opacity = 0.5;
    bool enabled() const { return blur > 0 && opacity > 0; }
};

struct Layer {
    std::string role;  // e.g. "Pregador"; resolved to `slot` when a template is applied
    int slot = -1;     // input slot 0..15, -1 = unassigned (draws a placeholder)
    Rect rect;
    FitMode fit = FitMode::Fill;
    double zoom = 1;  // Crop only, 1..4
    double cropX = 0.5, cropY = 0.5;
    double radius = 0;  // fraction of the layer's shorter side, 0..0.5
    Border border;
    Shadow shadow;
    double opacity = 1;
    int z = 0;  // draw order: ascending z, ties keep array order
};

struct Background {
    enum class Type { Solid, Gradient, Image } type = Type::Solid;
    Color from{16, 24, 40, 255}, to{30, 60, 110, 255};
    double angle = 90;      // degrees, 0 = left->right, 90 = top->bottom
    std::string imagePath;  // Image: BMP (24/32 bit) - see docs/TEMPLATES.md
};

struct Scene {
    std::string id, name;
    Background bg;
    std::vector<Layer> layers;
    std::vector<core::json::Value> overlays;  // plain data, e.g. {"type":"lowerthird","text":"{nome}"}
};

struct Template {
    std::string id, name, category, description;
    std::vector<std::string> roles;
    std::vector<Scene> scenes;
    bool builtin = false;
};

inline constexpr std::size_t kMaxLayers = 16;
inline constexpr std::size_t kMaxScenesPerTemplate = 16;
inline constexpr std::size_t kMaxOverlays = 16;
inline constexpr int kMaxSlot = 15;

// Parsing never throws; on failure returns false and fills `err` (pt-BR, user facing).
bool sceneFromJson(const core::json::Value& v, Scene& out, std::string& err);
core::json::Value sceneToJson(const Scene& s);
bool templateFromJson(const core::json::Value& v, Template& out, std::string& err);
core::json::Value templateToJson(const Template& t);

// Distinct role names in order of first appearance.
std::vector<std::string> rolesOf(const Scene& s);
std::vector<std::string> rolesOf(const std::vector<Scene>& scenes);

// Copy of `s` with every layer that has a role present in `roleMap` pointed at that slot (a role
// mapped to a negative or out-of-range slot leaves the layer unassigned).
Scene applyRoles(const Scene& s, const std::vector<std::pair<std::string, int>>& roleMap);

// Keeps ids/names to a safe, bounded form (no control chars, no '|', limited length).
std::string sanitizeText(const std::string& s, std::size_t maxLen);
std::string sanitizeId(const std::string& s);

}  // namespace hb::scene
