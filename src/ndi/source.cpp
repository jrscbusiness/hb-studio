#include "ndi/ndi.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>

#include "video/frame_ops.hpp"

namespace hb::ndi {

namespace {
using Clock = std::chrono::steady_clock;
constexpr int kCaptureTimeoutMs = 150;        // recvCaptureVideo poll timeout
constexpr auto kConnectingGrace = std::chrono::seconds(3);     // before "no signal yet" becomes NoSignal
constexpr auto kStaleAfter = std::chrono::milliseconds(1500);  // last frame older than this -> NoSignal
}  // namespace

struct NdiSource::Impl {
    mutable std::mutex mutex;
    std::thread thread;
    std::atomic<bool> stopFlag{false};

    // status
    bool createdOk = false;     // recvCreate() succeeded
    bool failed = false;        // hard failure (bad receiver, backend error, ...) -- terminal
    std::string error;
    Clock::time_point startedAt{};

    // frame state
    std::shared_ptr<const video::Frame> latest;
    Clock::time_point lastFrame{};
    int width = 0, height = 0;
    double fps = 0;
    std::uint64_t frameCount = 0;
    std::uint64_t fpsWindowCount = 0;
    Clock::time_point fpsWindowStart{};
};

NdiSource::NdiSource(NdiSourceInfo source, std::shared_ptr<NdiBackend> backend)
    : source_(std::move(source)), name_(source_.name), backend_(std::move(backend)), p_(std::make_unique<Impl>()) {}

NdiSource::~NdiSource() { stop(); }

void NdiSource::start() {
    if (p_->thread.joinable()) return;
    {
        std::lock_guard lock(p_->mutex);
        p_->createdOk = false;
        p_->failed = false;
        p_->error.clear();
        p_->latest.reset();
        p_->frameCount = 0;
        p_->fpsWindowCount = 0;
        p_->fps = 0;
        p_->width = p_->height = 0;
        p_->startedAt = Clock::now();
    }
    p_->stopFlag.store(false);
    p_->thread = std::thread([this] { run(); });
}

void NdiSource::stop() {
    if (!p_->thread.joinable()) return;
    p_->stopFlag.store(true);
    p_->thread.join();
}

void NdiSource::run() {
    // Permanent: nothing on our end will change it (the NDI Runtime itself is missing).
    auto fail = [this](std::string msg) {
        std::lock_guard lock(p_->mutex);
        p_->failed = true;
        p_->error = std::move(msg);
    };
    // Transient: the sender is off, hasn't started yet, or the connection hiccupped - all of
    // these commonly self-heal, so leave failed false (status() keeps reporting
    // Connecting/NoSignal, not a dead-end Error) and keep retrying in the background instead of
    // giving up on this receiver forever.
    auto retryMsg = [this](const std::string& msg) {
        std::lock_guard lock(p_->mutex);
        p_->error = msg;
    };
    auto sleepOrStop = [this](int ms) {
        const auto deadline = Clock::now() + std::chrono::milliseconds(ms);
        while (Clock::now() < deadline) {
            if (p_->stopFlag.load(std::memory_order_relaxed)) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return p_->stopFlag.load(std::memory_order_relaxed);
    };

    while (!p_->stopFlag.load(std::memory_order_relaxed)) {
        void* handle = nullptr;
        try {
            handle = backend_->recvCreate(source_);
        } catch (...) {
            handle = nullptr;
        }
        if (!handle) {
            if (backend_ && !backend_->available()) {
                fail("NDI Runtime não encontrado. Instale gratuitamente em ndi.video/tools para usar câmeras NDI.");
                return;
            }
            retryMsg("Não foi possível conectar à fonte NDI \"" + source_.name + "\" (desligada ou fora da rede?). Tentando novamente...");
            { std::lock_guard lock(p_->mutex); p_->createdOk = false; }
            if (sleepOrStop(1500)) break;
            continue;
        }
        {
            std::lock_guard lock(p_->mutex);
            p_->createdOk = true;
            p_->startedAt = Clock::now();   // restart the connecting grace period for this attempt
            p_->frameCount = 0;
            p_->fpsWindowCount = 0;
            p_->fps = 0;
        }

        // Watches for both failure shapes: never received a frame (sender never actually feeds
        // this receiver despite recvCreate succeeding), and received frames that then stop
        // arriving (sender went away mid-stream). Either way, destroy this receiver and loop back
        // to recreate it, instead of leaving status() stuck reporting NoSignal forever.
        bool stalled = false;
        while (!p_->stopFlag.load(std::memory_order_relaxed)) {
            std::vector<std::uint8_t> bgra;
            int w = 0, h = 0;
            double fps = 0;
            std::string err;
            bool ok = false;
            try {
                ok = backend_->recvCaptureVideo(handle, kCaptureTimeoutMs, bgra, w, h, fps, err);
            } catch (...) {
                err = "Erro inesperado ao receber vídeo NDI.";
            }
            if (!err.empty()) {
                retryMsg(err + " Tentando reconectar...");
                stalled = true;
                break;
            }
            if (!ok) {
                const auto now = Clock::now();
                std::lock_guard lock(p_->mutex);
                const bool everGotFrame = p_->frameCount > 0;
                const auto sinceLastFrame = everGotFrame ? (now - p_->lastFrame) : (now - p_->startedAt);
                const auto threshold = everGotFrame ? std::chrono::seconds(5) : std::chrono::seconds(6);
                if (sinceLastFrame > threshold) { stalled = true; break; }
                continue;  // plain timeout / recoverable hiccup: just try again
            }

            // Defensive against a hostile/buggy backend: only accept frames whose declared size
            // actually matches the buffer we got (inconsistent width/height/buffer across calls
            // must never cause an out-of-bounds read later in render()).
            if (w <= 0 || h <= 0 || w > 16384 || h > 16384) continue;
            const std::size_t need = static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4;
            if (bgra.size() != need) continue;

            auto frame = std::make_shared<video::Frame>();
            video::prepareFrame(*frame, w, h);
            std::memcpy(frame->data.data(), bgra.data(), need);

            const auto now = Clock::now();
            std::lock_guard lock(p_->mutex);
            p_->latest = std::move(frame);
            p_->lastFrame = now;
            p_->width = w;
            p_->height = h;
            ++p_->frameCount;
            if (p_->fpsWindowCount == 0) p_->fpsWindowStart = now;
            ++p_->fpsWindowCount;
            const double dt = std::chrono::duration<double>(now - p_->fpsWindowStart).count();
            if (dt >= 1.0) {
                p_->fps = static_cast<double>(p_->fpsWindowCount) / dt;
                p_->fpsWindowCount = 0;
            } else if (fps > 0) {
                p_->fps = fps;  // backend-reported rate, until we have our own measured window
            }
        }

        try {
            backend_->recvDestroy(handle);
        } catch (...) {
        }
        if (p_->stopFlag.load(std::memory_order_relaxed)) break;
        if (stalled) {
            { std::lock_guard lock(p_->mutex); p_->createdOk = false; }
            if (sleepOrStop(800)) break;
        }
    }
}

void NdiSource::render(video::Frame& out, std::uint64_t) const {
    std::shared_ptr<const video::Frame> f;
    Clock::time_point last;
    {
        std::lock_guard lock(p_->mutex);
        f = p_->latest;
        last = p_->lastFrame;
    }
    const bool fresh = f && (Clock::now() - last) < kStaleAfter;
    if (fresh) {
        try {
            video::fitFrame(*f, out);
        } catch (...) {
            // never let a malformed frame propagate as an exception from render()
        }
        return;
    }
    for (std::size_t i = 0; i + 3 < out.data.size(); i += 4) {
        out.data[i] = 43;
        out.data[i + 1] = 18;
        out.data[i + 2] = 7;
        out.data[i + 3] = 255;
    }
}

video::SourceStatus NdiSource::status() const {
    using S = video::SourceStatus::State;
    std::lock_guard lock(p_->mutex);
    video::SourceStatus s;
    s.width = p_->width;
    s.height = p_->height;
    s.fps = p_->fps;
    const auto now = Clock::now();
    if (p_->failed) {
        s.state = S::Error;
        s.message = p_->error;
    } else if (!p_->createdOk) {
        // Between reconnect attempts, error may hold a transient retry message set by run() -
        // surface it so the camera list shows what's actually happening.
        s.state = S::Connecting;
        s.message = p_->error;
    } else if (p_->frameCount == 0) {
        s.state = (now - p_->startedAt) < kConnectingGrace ? S::Connecting : S::NoSignal;
        if (s.state == S::NoSignal) s.message = "Sem imagem da fonte NDI.";
    } else if (now - p_->lastFrame > kStaleAfter) {
        s.state = S::NoSignal;
        s.fps = 0;
        s.message = "Sem imagem da fonte NDI.";
    } else {
        s.state = S::Online;
    }
    return s;
}

}  // namespace hb::ndi
