#pragma once
// Portable scene compositor. A Scene is "compiled" once per output size (geometry, rounded-corner
// masks, borders, blurred drop shadows, background); composing a frame is then a cheap
// scale-by-the-source + masked blend with no allocation in steady state.
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "scene/scene.hpp"
#include "video/program_output.hpp"

namespace hb::scene {

// Loads an image into a BGRA8 frame. The default loader reads uncompressed 24/32-bit BMP.
using ImageLoader = std::function<bool(const std::string& path, video::Frame& out)>;
bool loadBmp(const std::string& path, video::Frame& out);
bool decodeBmp(const std::uint8_t* data, std::size_t size, video::Frame& out);  // exposed for tests

struct CompiledLayer {
    int layer = 0;            // index into Scene::layers
    int slot = -1;
    int dx = 0, dy = 0, dw = 0, dh = 0;   // destination rect in output pixels (may extend past the frame)
    int srcW = 0, srcH = 0;               // size the source is asked to render at
    int offX = 0, offY = 0;               // dest (x,y) takes source pixel (x+offX, y+offY)
    int opacity256 = 256;
    bool direct = false;                  // bottom layer covering the frame exactly: rendered straight into the output
    std::vector<std::uint8_t> mask;       // dw*dh coverage (rounded corners); empty = fully opaque
    std::vector<std::uint8_t> ring;       // dw*dh border coverage; empty = no border
    Color ringColor;
    int shX = 0, shY = 0, shW = 0, shH = 0;  // shadow map rect in output pixels
    std::vector<std::uint8_t> shadow;        // shW*shH alpha (already scaled by opacity); empty = none
};

struct CompiledScene {
    int width = 0, height = 0;
    video::Frame bg;                       // background, fully opaque
    std::vector<CompiledLayer> layers;     // bottom -> top
    std::vector<double> aspects;           // per Scene::layers index, the aspect used (w/h)
};

// aspects[i] = native width/height of layer i's source (<= 0 means 16:9). Never throws; returns a
// compiled scene even for degenerate sizes (it will simply draw nothing).
std::shared_ptr<const CompiledScene> compileScene(const Scene& scene, int width, int height,
                                                  const std::vector<double>& aspects, const ImageLoader& loader = nullptr);

// Draws one layer's source into `frame` (already sized srcW x srcH). Return false for "no source":
// a placeholder panel is drawn instead.
using LayerRenderFn = std::function<bool(const Layer& layer, video::Frame& frame, std::uint64_t index)>;

void composeScene(const CompiledScene& cs, const Scene& scene, const LayerRenderFn& render, video::Frame& out,
                  std::uint64_t index);

// Deterministic placeholder colour for a role / slot (used for thumbnails and unassigned layers).
Color placeholderColor(const std::string& role, int slot);

// Renders the scene with placeholder panels instead of cameras (picker thumbnails, tests).
void renderThumbnail(const Scene& scene, int width, int height, video::Frame& out);

}  // namespace hb::scene
