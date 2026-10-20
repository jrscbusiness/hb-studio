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
    if (clock_.running() && bus_) bus_->publish({"program.started", {}});  // invalid fps => not started
}

void ProgramOutput::stop() {
    if (!clock_.running()) return;
    clock_.stop();
    if (bus_) bus_->publish({"program.stopped", {}});
}

ProgramOutput::ConsumerId ProgramOutput::addConsumer(Consumer consumer) {
    auto e = std::make_shared<Entry>();
    e->fn = std::move(consumer);
    std::lock_guard lock(mutex_);
    e->id = nextConsumer_++;
    consumers_.push_back(e);
    return e->id;
}

void ProgramOutput::removeConsumer(ConsumerId id) {
    std::shared_ptr<Entry> e;
    {
        std::lock_guard lock(mutex_);
        auto it = std::find_if(consumers_.begin(), consumers_.end(), [id](const auto& c) { return c->id == id; });
        if (it == consumers_.end()) return;
        e = *it;
        consumers_.erase(it);
    }
    // In-flight callbacks were counted while holding mutex_ (see deliver()), so after this
    // wait none is running. A consumer removing itself from its own callback must not wait.
    std::unique_lock lock(e->m);
    if (e->runner == std::this_thread::get_id()) return;
    e->cv.wait(lock, [&] { return e->inflight == 0; });
}

std::shared_ptr<const Frame> ProgramOutput::latest() const {
    std::lock_guard lock(mutex_);
    return latest_;
}

void ProgramOutput::deliver(const std::shared_ptr<const Frame>& f) {
    std::vector<std::shared_ptr<Entry>> list;
    {
        std::lock_guard lock(mutex_);
        list = consumers_;
        for (auto& e : list) {                       // count in-flight while the list is stable
            std::lock_guard el(e->m);
            ++e->inflight;
        }
    }
    for (auto& e : list) {
        {
            std::lock_guard el(e->m);
            e->runner = std::this_thread::get_id();
        }
        try {
            e->fn(f);
        } catch (...) {
            consumerErrors_.fetch_add(1);
        }
        std::lock_guard el(e->m);
        e->runner = std::thread::id();
        --e->inflight;
        e->cv.notify_all();
    }
}

void ProgramOutput::onTick(std::uint64_t index, core::SteadyClock::time_point deadline) {
    Renderer renderer;
    {
        std::lock_guard lock(mutex_);
        renderer = renderer_;
    }
    auto frame = pool_.acquire();
    bool failed = false;
    try {
        renderer(*frame, index);
    } catch (...) {
        failed = true;
        rendererErrors_.fetch_add(1);
    }
    if (failed) {
        // A faulty renderer must not starve the consumers: repeat the last good frame
        // (not counted as produced), or send black if there is none yet.
        if (auto prev = latest()) {
            repeated_.fetch_add(1);
            deliver(prev);
            return;
        }
        std::fill(frame->data.begin(), frame->data.end(), std::uint8_t{0});
    }
    frame->index = index;
    frame->ptsNs = std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - origin_).count();

    std::shared_ptr<const Frame> published = frame;
    {
        std::lock_guard lock(mutex_);
        latest_ = published;
    }
    produced_.fetch_add(1);
    deliver(published);
}

}  // namespace hb::video
