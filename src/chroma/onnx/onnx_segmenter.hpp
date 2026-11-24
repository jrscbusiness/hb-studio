#pragma once
// ONNX Runtime backed ISegmenter (Windows, optional: built only when HB_ORT_DIR points to an
// extracted official onnxruntime-win-x64 release; see cmake/modules/chroma.cmake). CPU provider.
// NOT validated on real hardware by the author - see docs/CHROMA.md.
#include <memory>
#include <string>

#include "chroma/segmenter.hpp"

namespace hb::chroma {

// Loads the model at `modelPath` (already SHA-256 verified by ModelStore). Null + `err` on failure
// (missing onnxruntime.dll next to the exe, incompatible model, ...). Never throws.
std::shared_ptr<ISegmenter> createOnnxSegmenter(const std::string& modelPath, std::string& err);

}  // namespace hb::chroma
