#pragma once
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include "chroma/segmenter.hpp"
#include "video/frame_ops.hpp"

namespace hbt {

inline hb::video::Frame solid(int w, int h, std::uint8_t b, std::uint8_t g, std::uint8_t r) {
    hb::video::Frame f;
    hb::video::prepareFrame(f, w, h);
    for (std::size_t i = 0; i + 3 < f.data.size(); i += 4) { f.data[i] = b; f.data[i + 1] = g; f.data[i + 2] = r; f.data[i + 3] = 255; }
    return f;
}
inline void setPx(hb::video::Frame& f, int x, int y, std::uint8_t b, std::uint8_t g, std::uint8_t r) {
    auto* p = f.data.data() + static_cast<std::size_t>(y) * f.stride + x * 4;
    p[0] = b; p[1] = g; p[2] = r; p[3] = 255;
}
inline const std::uint8_t* px(const hb::video::Frame& f, int x, int y) {
    return f.data.data() + static_cast<std::size_t>(y) * f.stride + x * 4;
}

// Fake model: "person" = the left half of its input, with a configurable delay / failure.
class FakeSegmenter final : public hb::chroma::ISegmenter {
public:
    int delayMs = 0;
    std::atomic<bool> fail{false};
    std::atomic<int> calls{0};
    int inputWidth() const override { return 32; }
    int inputHeight() const override { return 18; }
    bool segment(const hb::video::Frame& in, hb::chroma::Mask& out, std::string& err) override {
        ++calls;
        if (delayMs) std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
        if (fail) { err = "boom"; return false; }
        out.resize(in.width, in.height);
        for (int y = 0; y < in.height; ++y)
            for (int x = 0; x < in.width; ++x) out.data[static_cast<std::size_t>(y) * in.width + x] = x < in.width / 2 ? 255 : 0;
        return true;
    }
};

}  // namespace hbt
