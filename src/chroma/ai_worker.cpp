#include "chroma/ai_worker.hpp"

#include <algorithm>

#include "video/frame_ops.hpp"

namespace hb::chroma {

PostParams makePostParams(const Settings& s) {
    const QualityParams q = qualityParams(s.quality);
    PostParams p;
    const float centre = 255.f * (0.30f + 0.40f * static_cast<float>(std::clamp(s.tolerance, 0, 100)) / 100.f);
    const float half = 255.f * (0.03f + 0.27f * static_cast<float>(std::clamp(s.softness, 0, 100)) / 100.f);
    p.levelLo = std::clamp(static_cast<int>(centre - half), 0, 253);
    p.levelHi = std::clamp(static_cast<int>(centre + half), p.levelLo + 1, 255);
    p.erodePasses = q.erode;
    p.featherRadius = q.feather;
    p.emaNew = q.emaNew;
    p.minIntervalMs = q.minIntervalMs;
    return p;
}

struct AiMaskWorker::State {
    std::shared_ptr<ISegmenter> seg;
    mutable std::mutex m;
    std::condition_variable cv;
    video::Frame pending;
    bool hasPending = false;
    bool busy = false;
    bool stop = false;
    bool failed = false;
    PostParams params;
    std::shared_ptr<const Mask> latest;
    Mask acc;   // worker-private temporal accumulator
    Stats stats;
    std::int64_t lastAcceptNs = -(std::int64_t{1} << 60);
    int consecutiveFailures = 0;

    static std::int64_t nowNs() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    void loop() {
        video::Frame work, small;
        while (true) {
            PostParams pp;
            {
                std::unique_lock lock(m);
                cv.wait(lock, [&] { return stop || hasPending; });
                if (stop) return;
                std::swap(work, pending);
                hasPending = false;
                busy = true;
                pp = params;
            }
            const auto t0 = std::chrono::steady_clock::now();
            Mask raw;
            std::string err;
            bool ok = false;
            try {
                video::downscaleBox(work, small, seg->inputWidth(), seg->inputHeight());
                ok = small.width == seg->inputWidth() && small.height == seg->inputHeight() && seg->segment(small, raw, err) &&
                     raw.valid();
                if (!ok && err.empty()) err = "segmentador devolveu máscara inválida";
            } catch (const std::exception& e) {
                err = std::string("exceção no segmentador: ") + e.what();
            } catch (...) {
                err = "exceção desconhecida no segmentador";
            }
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

            if (ok) {
                // Post-process at mask resolution (tiny): levels -> erode -> feather -> temporal EMA.
                applyLevels(raw, pp.levelLo, pp.levelHi);
                erode(raw, pp.erodePasses);
                boxBlur(raw, pp.featherRadius);
                emaBlend(acc, raw, pp.emaNew);
                auto published = std::make_shared<const Mask>(acc);
                std::lock_guard lock(m);
                latest = std::move(published);
                consecutiveFailures = 0;
                ++stats.processed;
                stats.inferMsLast = ms;
                stats.inferMsAvg = stats.processed == 1 ? ms : stats.inferMsAvg * 0.9 + ms * 0.1;
                busy = false;
            } else {
                std::lock_guard lock(m);
                ++stats.failures;
                stats.message = err;
                if (++consecutiveFailures >= 5) {
                    failed = stats.failed = true;   // stop hammering a broken model/runtime
                    stats.message = "IA desativada após falhas repetidas: " + err;
                    busy = false;
                    return;
                }
                busy = false;
            }
        }
    }
};

AiMaskWorker::AiMaskWorker(std::shared_ptr<ISegmenter> seg, PostParams p) : st_(std::make_shared<State>()) {
    st_->seg = std::move(seg);
    st_->params = p;
}

AiMaskWorker::~AiMaskWorker() {
    if (th_.joinable()) {
        {
            std::lock_guard lock(st_->m);
            st_->stop = true;
        }
        st_->cv.notify_all();
        th_.detach();   // the thread owns a shared_ptr<State>; it exits at the next wake-up
    }
}

void AiMaskWorker::start() {
    if (th_.joinable() || stopped_ || !st_->seg) return;
    auto st = st_;
    th_ = std::thread([st] { st->loop(); });
}

void AiMaskWorker::stop() {
    {
        std::lock_guard lock(st_->m);
        st_->stop = true;
    }
    st_->cv.notify_all();
    if (th_.joinable()) th_.join();
    stopped_ = true;
}

bool AiMaskWorker::wantsFrame() const {
    std::lock_guard lock(st_->m);
    if (st_->stop || st_->failed || st_->busy || st_->hasPending) return false;
    return State::nowNs() - st_->lastAcceptNs >= static_cast<std::int64_t>(st_->params.minIntervalMs) * 1000000;
}

bool AiMaskWorker::submit(const video::Frame& f) {
    if (f.width <= 0 || f.height <= 0 ||
        f.data.size() < static_cast<std::size_t>(f.stride) * static_cast<std::size_t>(f.height))
        return false;
    {
        std::lock_guard lock(st_->m);
        if (st_->stop || st_->failed) return false;
        if (st_->busy || st_->hasPending) {
            ++st_->stats.dropped;
            return false;
        }
        const std::int64_t now = State::nowNs();
        if (now - st_->lastAcceptNs < static_cast<std::int64_t>(st_->params.minIntervalMs) * 1000000) return false;
        st_->lastAcceptNs = now;
        auto& p = st_->pending;
        p.width = f.width;
        p.height = f.height;
        p.stride = f.stride;
        p.format = f.format;
        p.data = f.data;   // reuses capacity after the first frames
        st_->hasPending = true;
    }
    st_->cv.notify_one();
    return true;
}

std::shared_ptr<const Mask> AiMaskWorker::latest() const {
    std::lock_guard lock(st_->m);
    return st_->latest;
}

void AiMaskWorker::setParams(const PostParams& p) {
    std::lock_guard lock(st_->m);
    st_->params = p;
}

AiMaskWorker::Stats AiMaskWorker::stats() const {
    std::lock_guard lock(st_->m);
    return st_->stats;
}

int AiMaskWorker::inputWidth() const { return st_->seg ? st_->seg->inputWidth() : 0; }
int AiMaskWorker::inputHeight() const { return st_->seg ? st_->seg->inputHeight() : 0; }

}  // namespace hb::chroma
