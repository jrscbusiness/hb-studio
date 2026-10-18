#include "video/program_output.hpp"

#include <algorithm>

namespace hb::video {

std::shared_ptr<Frame> FramePool::acquire() {
    std::unique_ptr<Frame> f;
    {
        std::lock_guard lock(state_->mutex);
        if (!state_->idle.empty()) {
            f = std::move(state_->idle.back());
            state_->idle.pop_back();
        }
    }
    if (!f) {
        f = std::make_unique<Frame>();
        f->width = width_;
        f->height = height_;
        f->stride = width_ * 4;
        f->data.resize(static_cast<std::size_t>(f->stride) * height_);
    }
    // The deleter returns the buffer to the pool; it holds the pool state
    // alive, so frames may safely outlive the pool object.
    auto state = state_;
    return std::shared_ptr<Frame>(f.release(), [state](Frame* p) {
        std::lock_guard lock(state->mutex);
        state->idle.emplace_back(p);
    });
}

std::size_t FramePool::idleCount() const {
    std::lock_guard lock(state_->mutex);
    return state_->idle.size();
}

void renderTestPattern(Frame& f, std::uint64_t index) {
    // 7 classic bars: white, yellow, cyan, green, magenta, red, blue (BGRA).
    static const std::uint8_t bars[7][4] = {
        {235, 235, 235, 255}, {16, 235, 235, 255}, {235, 235, 16, 255}, {16, 235, 16, 255},
        {235, 16, 235, 255},  {16, 16, 235, 255},  {235, 16, 16, 255},
    };
    const int markerW = std::max(4, f.width / 40);
    const int markerX = static_cast<int>((index * 8) % static_cast<std::uint64_t>(f.width));
    for (int y = 0; y < f.height; ++y) {
        auto* row = f.data.data() + static_cast<std::size_t>(y) * f.stride;
        for (int x = 0; x < f.width; ++x) {
            const bool marker = y > f.height * 3 / 4 && x >= markerX && x < markerX + markerW;
            const auto* c = marker ? bars[0] : bars[x * 7 / f.width];
            auto* px = row + static_cast<std::size_t>(x) * 4;
            px[0] = c[0]; px[1] = c[1]; px[2] = c[2]; px[3] = c[3];
        }
    }
    f.index = index;
}

ProgramOutput::ProgramOutput(ProgramFormat format, core::EventBus* bus)
    : format_(format), bus_(bus), pool_(format.width, format.height),
      renderer_(renderTestPattern) {}

void ProgramOutput::setRenderer(Renderer renderer) {
    std::lock_guard lock(mutex_);
    renderer_ = renderer ? std::move(renderer) : Renderer(renderTestPattern);
}

void ProgramOutput::start() {
    if (clock_.running()) return;
    origin_ = core::SteadyClock::now();
    clock_.start(format_.fps, [this](std::uint64_t i, core::SteadyClock::time_point d) { onTick(i, d); });
    if (bus_) bus_->publish({"program.started", {}});
}

void ProgramOutput::stop() {
    if (!clock_.running()) return;
    clock_.stop();
    if (bus_) bus_->publish({"program.stopped", {}});
}

ProgramOutput::ConsumerId ProgramOutput::addConsumer(Consumer consumer) {
    std::lock_guard lock(mutex_);
    const auto id = nextConsumer_++;
    consumers_.emplace_back(id, std::move(consumer));
    return id;
}

void ProgramOutput::removeConsumer(ConsumerId id) {
    std::lock_guard lock(mutex_);
    consumers_.erase(std::remove_if(consumers_.begin(), consumers_.end(),
                                    [id](const auto& c) { return c.first == id; }),
                     consumers_.end());
}

std::shared_ptr<const Frame> ProgramOutput::latest() const {
    std::lock_guard lock(mutex_);
    return latest_;
}

void ProgramOutput::onTick(std::uint64_t index, core::SteadyClock::time_point deadline) {
    Renderer renderer;
    {
        std::lock_guard lock(mutex_);
        renderer = renderer_;
    }
    auto frame = pool_.acquire();
    try {
        renderer(*frame, index);
    } catch (...) {
        // A faulty renderer must not kill the output: repeat the last frame.
        rendererErrors_.fetch_add(1);
        if (auto prev = latest()) {
            produced_.fetch_add(1);
            return;
        }
    }
    frame->index = index;
    frame->ptsNs = std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - origin_).count();

    std::shared_ptr<const Frame> published = frame;
    std::vector<std::pair<ConsumerId, Consumer>> consumers;
    {
        std::lock_guard lock(mutex_);
        latest_ = published;
        consumers = consumers_;
    }
    produced_.fetch_add(1);
    for (auto& [id, fn] : consumers) {
        try {
            fn(published);
        } catch (...) {
            consumerErrors_.fetch_add(1);
        }
    }
}

}  // namespace hb::video
