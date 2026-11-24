#pragma once
// Decodes an image file (PNG/JPEG/BMP/GIF/TIFF/WebP if the codec is installed) into an opaque BGRA
// frame for the "imagem" background replacement. Windows only (WIC).
#include <string>

#include "video/program_output.hpp"

namespace hb::chroma {

// `utf8Path` is a file path. False + `err` (pt-BR) on any failure; `out` is untouched on failure.
// Rejects files over 50 MB and images with a side over 8192 px or under 1 px.
bool loadImageWic(const std::string& utf8Path, video::Frame& out, std::string& err);

}  // namespace hb::chroma
