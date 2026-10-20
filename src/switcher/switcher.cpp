#include "switcher/switcher.hpp"

#include <algorithm>
#include <string>

namespace hb::switcher {

Switcher::Switcher(std::vector<std::shared_ptr<const video::Source>> sources, core::EventBus* bus)
    : sources_(std::move(sources)), bus_(bus) {
    if (sources_.size() < 2) pvw_ = 0;
}

void Switcher::publish(const char* topic, int a, int b) const {
    if (!bus_) return;
    bus_->publish({topic, {{"pvw", std::to_string(a)}, {"pgm", std::to_string(b)}}});
}

Switcher::State Switcher::state(Clock::time_point now) const {
    std::lock_guard lock(mutex_);
    State s{pvw_, pgm_, transitioning_, 0.f};
    if (transitioning_) {
        const float p = std::chrono::duration<float, std::milli>(now - start_).count() /
                        static_cast<float>(duration_.count());
        s.progress = std::clamp(p, 0.f, 1.f);
    }
    return s;
}

bool Switcher::setPreview(int i) {
    {
        std::lock_guard lock(mutex_);
        if (i < 0 || i >= sourceCount() || transitioning_) return false;
        pvw_ = i;
    }
    publish("switcher.previewChanged", i, state().pgm);
    return true;
}

bool Switcher::setProgram(int i) {
    int pvw;
    {
        std::lock_guard lock(mutex_);
        if (i < 0 || i >= sourceCount() || transitioning_) return false;
        pgm_ = i;
        pvw = pvw_;
    }
    publish("switcher.programChanged", pvw, i);
    return true;
}

bool Switcher::cut() {
    int pvw, pgm;
    {
        std::lock_guard lock(mutex_);
        if (transitioning_ || pvw_ == pgm_) return false;
        std::swap(pvw_, pgm_);
        pvw = pvw_;
        pgm = pgm_;
    }
    publish("switcher.programChanged", pvw, pgm);
    return true;
}

bool Switcher::startDissolve(int ms, Clock::time_point now) {
    if (ms <= 0) return cut();
    int pvw, pgm;
    {
        std::lock_guard lock(mutex_);
        if (transitioning_ || pvw_ == pgm_) return false;
        transitioning_ = true;
        start_ = now;
        duration_ = std::chrono::milliseconds(ms);
        pvw = pvw_;
        pgm = pgm_;
    }
    publish("transition.started", pvw, pgm);
    return true;
}

void Switcher::renderPreview(video::Frame& out, std::uint64_t index) const {
    int i;
    {
        std::lock_guard lock(mutex_);
        i = pvw_;
    }
    sources_[static_cast<std::size_t>(i)]->render(out, index);
}

void Switcher::renderProgram(video::Frame& out, std::uint64_t index, Clock::time_point now) {
    int pvw, pgm;
    bool transitioning;
    float progress = 0.f;
    {
        std::lock_guard lock(mutex_);
        pvw = pvw_;
        pgm = pgm_;
        transitioning = transitioning_;
        if (transitioning)
            progress = std::clamp(std::chrono::duration<float, std::milli>(now - start_).count() /
                                      static_cast<float>(duration_.count()), 0.f, 1.f);
    }
    if (!transitioning) {
        sources_[static_cast<std::size_t>(pgm)]->render(out, index);
        return;
    }
    video::prepareFrame(scratchA_, out.width, out.height);
    video::prepareFrame(scratchB_, out.width, out.height);
    sources_[static_cast<std::size_t>(pgm)]->render(scratchA_, index);
    sources_[static_cast<std::size_t>(pvw)]->render(scratchB_, index);
    video::mixFrames(scratchA_, scratchB_, out, static_cast<int>(progress * 256.f + 0.5f));

    if (progress >= 1.f) {
        {
            std::lock_guard lock(mutex_);
            std::swap(pvw_, pgm_);
            transitioning_ = false;
        }
        publish("transition.finished", pgm, pvw);
        publish("switcher.programChanged", pgm, pvw);
    }
}

}  // namespace hb::switcher
