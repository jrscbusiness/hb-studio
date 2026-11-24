#include "chroma/chroma_filter.hpp"

#include <algorithm>
#include <chrono>

namespace hb::chroma {

namespace {
constexpr int kBlurReuseFrames = 3;   // the blurred backdrop is re-derived every Nth call per size
constexpr std::size_t kMaxCache = 4;  // distinct render sizes cached (PGM, PVW, tiles...)
}  // namespace

void ChromaFilter::rebuildWorkerLocked() {
    if (s_.mode == Mode::Ai && seg_) {
        if (!worker_) {
            worker_ = std::make_shared<AiMaskWorker>(seg_, makePostParams(s_));
            worker_->start();
        } else {
            worker_->setParams(makePostParams(s_));
        }
    } else {
        worker_.reset();   // AiMaskWorker's destructor never blocks (see ai_worker.cpp)
    }
}

void ChromaFilter::setSettings(const Settings& s) {
    std::lock_guard lock(m_);
    if (s.image != s_.image) imageCache_.clear();
    if (s.blur != s_.blur || s.repl != s_.repl) blurCache_.clear();
    s_ = s;
    rebuildWorkerLocked();
}

Settings ChromaFilter::settings() const {
    std::lock_guard lock(m_);
    return s_;
}

void ChromaFilter::setSegmenter(std::shared_ptr<ISegmenter> seg) {
    std::lock_guard lock(m_);
    if (seg_ == seg) return;
    seg_ = std::move(seg);
    worker_.reset();
    rebuildWorkerLocked();
}

void ChromaFilter::setImage(std::shared_ptr<const video::Frame> img) {
    std::lock_guard lock(m_);
    image_ = std::move(img);
    imageCache_.clear();
}

void ChromaFilter::shutdown() {
    std::shared_ptr<AiMaskWorker> w;
    {
        std::lock_guard lock(m_);
        w = std::move(worker_);
    }
    if (w) w->stop();
}

std::shared_ptr<const video::Frame> ChromaFilter::backgroundFor(const video::Frame& frame, const Settings& s,
                                                                std::shared_ptr<const video::Frame> image) {
    if (s.repl == ReplKind::Blur) {
        {
            std::lock_guard lock(m_);
            for (auto& e : blurCache_)
                if (e.w == frame.width && e.h == frame.height && e.frame && ++e.age < kBlurReuseFrames) return e.frame;
        }
        auto f = std::make_shared<video::Frame>();
        makeBlurBackground(frame, s.blur, *f);
        std::lock_guard lock(m_);
        for (auto& e : blurCache_)
            if (e.w == frame.width && e.h == frame.height) { e.frame = f; e.age = 0; return f; }
        if (blurCache_.size() >= kMaxCache) blurCache_.erase(blurCache_.begin());
        blurCache_.push_back({frame.width, frame.height, 0, f});
        return f;
    }
    if (s.repl == ReplKind::Image && image) {
        {
            std::lock_guard lock(m_);
            for (auto& e : imageCache_)
                if (e.w == frame.width && e.h == frame.height && e.frame) return e.frame;
        }
        auto f = std::make_shared<video::Frame>();
        coverScale(*image, *f, frame.width, frame.height);
        std::lock_guard lock(m_);
        if (imageCache_.size() >= kMaxCache) imageCache_.erase(imageCache_.begin());
        imageCache_.push_back({frame.width, frame.height, 0, f});
        return f;
    }
    return nullptr;
}

void ChromaFilter::apply(video::Frame& frame) {
    Settings s;
    std::shared_ptr<AiMaskWorker> worker;
    std::shared_ptr<const video::Frame> image;
    {
        std::lock_guard lock(m_);
        if (s_.mode == Mode::Off) return;
        s = s_;
        worker = worker_;
        image = image_;
        maxWidthSeen_ = std::max(maxWidthSeen_, frame.width);
    }
    if (frame.width <= 0 || frame.height <= 0 ||
        frame.data.size() < static_cast<std::size_t>(frame.stride) * static_cast<std::size_t>(frame.height))
        return;

    const auto t0 = std::chrono::steady_clock::now();
    thread_local Mask scratch;   // reused across calls on this thread: no per-frame allocation

    std::shared_ptr<const Mask> aiMask;
    const Mask* matte = nullptr;
    Spill spill = Spill::None;
    if (s.mode == Mode::Green || s.mode == Mode::Blue) {
        KeyParams kp;
        kp.mode = s.mode;
        kp.tolerance = s.tolerance;
        kp.softness = s.softness;
        computeKeyMask(frame, kp, scratch);
        matte = &scratch;
        spill = s.mode == Mode::Green ? Spill::Green : Spill::Blue;
    } else if (s.mode == Mode::Ai) {
        if (!worker) return;   // no model/runtime: pass the frame through untouched (never a black screen)
        // Only large renders feed the model (tiles are tiny and would starve it of detail); the
        // wantsFrame() check skips even the frame copy when the worker is busy or throttled.
        int maxW;
        { std::lock_guard lock(m_); maxW = maxWidthSeen_; }
        if (frame.width * 2 >= maxW && worker->wantsFrame()) worker->submit(frame);
        aiMask = worker->latest();
        if (!aiMask) return;   // first mask not ready yet: show the camera as-is
        matte = aiMask.get();
    }
    if (!matte || !matte->valid()) return;

    Background bg;
    bg.kind = s.repl;
    bg.color = s.color;
    std::shared_ptr<const video::Frame> backdrop;
    if (s.repl == ReplKind::Blur || s.repl == ReplKind::Image) {
        backdrop = backgroundFor(frame, s, image);
        bg.image = backdrop.get();
    }
    compositeMatte(frame, *matte, bg, spill, s.spill);

    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::lock_guard lock(m_);
    applyMsAvg_ = applyMsAvg_ == 0 ? ms : applyMsAvg_ * 0.9 + ms * 0.1;
}

ChromaFilter::Stats ChromaFilter::stats() const {
    Stats st;
    std::shared_ptr<AiMaskWorker> w;
    {
        std::lock_guard lock(m_);
        st.applyMsAvg = applyMsAvg_;
        w = worker_;
        st.aiWaiting = s_.mode == Mode::Ai && !seg_;
    }
    if (w) {
        const auto ws = w->stats();
        st.aiActive = true;
        st.inferMsAvg = ws.inferMsAvg;
        st.inferences = ws.processed;
        st.dropped = ws.dropped;
        st.aiFailed = ws.failed;
        st.aiMessage = ws.message;
    }
    return st;
}

}  // namespace hb::chroma
