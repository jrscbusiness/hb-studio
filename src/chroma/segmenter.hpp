#pragma once
// Pure segmentation interface. The only implementation that does real inference lives in
// src/chroma/onnx/ (Windows + ONNX Runtime, optional); tests use a fake.
#include <string>

#include "chroma/types.hpp"
#include "video/program_output.hpp"

namespace hb::chroma {

class ISegmenter {
public:
    virtual ~ISegmenter() = default;
    // Fixed network input size; the worker box-downscales every frame to exactly this size.
    virtual int inputWidth() const = 0;
    virtual int inputHeight() const = 0;
    // `in` is BGRA8 of exactly inputWidth x inputHeight. Writes a person-probability matte
    // (255 = person) of ANY size into `out` (the compositor upsamples). MUST be thread-safe: one
    // segmenter instance is shared by the per-slot workers. Returns false and sets `err` on failure.
    virtual bool segment(const video::Frame& in, Mask& out, std::string& err) = 0;
};

}  // namespace hb::chroma
