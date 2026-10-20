#pragma once
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include "video/program_output.hpp"

namespace hb::video {

// A video input. The caller chooses the render size (Preview/Multiview tiles
// ask for small frames, Program asks for full size): sources never decide it.
class Source {
public:
    virtual ~Source() = default;
    virtual const std::string& name() const = 0;
    virtual bool isTestSource() const = 0;
    // Fills `frame` (already sized by the caller) for tick `index`.
    virtual void render(Frame& frame, std::uint64_t index) const = 0;
};

// Procedural source used until real cameras exist (Phases 2-6). It is always
// labelled "TESTE" in the UI: it is NOT a camera.
class SyntheticSource final : public Source {
public:
    SyntheticSource(int number, std::string name) : number_(number), name_(std::move(name)) {}
    const std::string& name() const override { return name_; }
    bool isTestSource() const override { return true; }
    void render(Frame& frame, std::uint64_t index) const override;
    int number() const { return number_; }

private:
    // Static layer (gradient + grid) is rendered once per output size and copied per frame;
    // only the moving parts (circle, progress bar, digit) are drawn every tick.
    std::shared_ptr<const Frame> background(int w, int h) const;

    int number_;
    std::string name_;
    mutable std::mutex cacheMutex_;
    mutable std::map<std::pair<int, int>, std::shared_ptr<const Frame>> cache_;
};

}  // namespace hb::video
