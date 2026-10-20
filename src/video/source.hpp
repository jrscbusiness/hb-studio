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

struct SourceStatus {
    enum class State { Connecting, Online, NoSignal, Error };
    State state = State::Online;
    int width = 0, height = 0;   // native resolution of the device
    double fps = 0;              // measured
    std::string message;         // human readable detail (errors)
};

// A real input device (webcam, virtual camera, ...). render() must be cheap and never block:
// it scales the newest received frame to the size the caller asked for.
class LiveSource : public Source {
public:
    bool isTestSource() const override { return false; }
    virtual SourceStatus status() const = 0;
};

// One of the 8 input slots (CAM 1..8). It forwards to the assigned device, or to the built-in test
// source when nothing is assigned. The switcher holds slots forever; devices come and go behind them.
class SlotSource final : public Source {
public:
    SlotSource(int number, std::shared_ptr<const Source> fallback)
        : name_("CAM " + std::to_string(number)), fallback_(std::move(fallback)) {}

    const std::string& name() const override { return name_; }
    bool isTestSource() const override { return !live(); }
    void render(Frame& frame, std::uint64_t index) const override {
        std::shared_ptr<const Source> s;
        {
            std::lock_guard lock(mutex_);
            s = inner_ ? inner_ : fallback_;
        }
        s->render(frame, index);
    }

    void assign(std::shared_ptr<const LiveSource> device, std::string deviceId, std::string deviceName) {
        std::lock_guard lock(mutex_);
        inner_ = device;
        live_ = device;
        id_ = std::move(deviceId);
        label_ = std::move(deviceName);
    }
    void clear() {
        std::lock_guard lock(mutex_);
        inner_.reset();
        live_.reset();
        id_.clear();
        label_.clear();
    }

    struct Info {
        bool live = false;
        std::string deviceId, label;
        SourceStatus status;
    };
    Info info() const {
        std::shared_ptr<const LiveSource> l;
        Info i;
        {
            std::lock_guard lock(mutex_);
            l = live_;
            i.deviceId = id_;
            i.label = label_;
        }
        i.live = static_cast<bool>(l);
        if (l) i.status = l->status();
        return i;
    }
    bool live() const {
        std::lock_guard lock(mutex_);
        return static_cast<bool>(live_);
    }

private:
    std::string name_;
    std::shared_ptr<const Source> fallback_;
    mutable std::mutex mutex_;
    std::shared_ptr<const Source> inner_;
    std::shared_ptr<const LiveSource> live_;
    std::string id_, label_;
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
