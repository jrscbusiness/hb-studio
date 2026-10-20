#pragma once
#include <string>

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
    int number_;
    std::string name_;
};

}  // namespace hb::video
