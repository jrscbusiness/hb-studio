#pragma once
// A fully in-process, deterministic NdiBackend for tests: no network, no Runtime, no hardware.
// Lets tests simulate a well-behaved source as well as every hostile behaviour ndi.hpp's contract
// has to survive (timeouts, hard errors, inconsistent frame sizes between calls).
#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include "ndi/ndi.hpp"

namespace hb::ndi::test {

class FakeNdiBackend final : public NdiBackend {
public:
    bool available() const override { return available_.load(); }
    std::string runtimeVersion() const override { return "fake-1.0"; }

    // ---- discovery side --------------------------------------------------------------------
    void* findCreate() override {
        ++findCreateCalls;
        if (!available_.load() || failFindCreate.load()) return nullptr;
        return &findToken_;
    }
    void findDestroy(void* handle) override {
        if (handle == &findToken_) ++findDestroyCalls;
    }
    std::vector<NdiSourceInfo> findCurrentSources(void* handle) override {
        if (handle != &findToken_) return {};
        std::lock_guard lock(mutex_);
        return sources_;
    }

    void setSources(std::vector<NdiSourceInfo> s) {
        std::lock_guard lock(mutex_);
        sources_ = std::move(s);
    }

    // ---- receive side ------------------------------------------------------------------------
    void* recvCreate(const NdiSourceInfo& source) override {
        ++recvCreateCalls;
        std::lock_guard lock(mutex_);
        lastRequestedSource_ = source;
        if (!available_.load() || failRecvCreate.load()) return nullptr;
        return &recvToken_;
    }
    void recvDestroy(void* handle) override {
        if (handle == &recvToken_) ++recvDestroyCalls;
    }

    // Behaviour knobs, all atomic so the test thread can flip them while NdiSource's background
    // thread is running.
    std::atomic<bool> available_{true};
    std::atomic<bool> failFindCreate{false};
    std::atomic<bool> failRecvCreate{false};
    std::atomic<int> mode{0};  // 0=deliver frames, 1=always timeout, 2=hard error, 3=inconsistent sizes
    std::atomic<int> frameWidth{4}, frameHeight{2};
    std::atomic<int> callsBeforeDelivering{0};  // first N calls time out, then frames start arriving

    std::atomic<int> findCreateCalls{0}, findDestroyCalls{0};
    std::atomic<int> recvCreateCalls{0}, recvDestroyCalls{0}, captureCalls{0}, framesDelivered{0};

    bool recvCaptureVideo(void* handle, int /*timeoutMs*/, std::vector<std::uint8_t>& bgra, int& width, int& height,
                          double& fps, std::string& error) override {
        ++captureCalls;
        if (handle != &recvToken_) {
            error = "handle inválido";
            return false;
        }
        if (mode.load() == 2) {
            error = "falha irrecuperável simulada";
            return false;
        }
        if (mode.load() == 1) return false;  // perpetual timeout, no error
        if (captureCalls.load() <= callsBeforeDelivering.load()) return false;

        const int w = frameWidth.load();
        const int h = frameHeight.load();
        width = w;
        height = h;
        fps = 30.0;
        if (mode.load() == 3) {
            // Hostile: claims a size but hands back a buffer of a different size, and alternates
            // which is wrong call to call -- NdiSource must reject these, never read/write OOB.
            const bool shrink = (captureCalls.load() % 2) == 0;
            const std::size_t n = shrink ? static_cast<std::size_t>(w) * h * 4 / 2
                                          : static_cast<std::size_t>(w) * h * 4 + 7;
            bgra.assign(n, 0x7F);
            ++framesDelivered;
            return true;
        }
        bgra.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4, 0);
        for (std::size_t i = 0; i + 3 < bgra.size(); i += 4) {
            bgra[i] = 10;
            bgra[i + 1] = 20;
            bgra[i + 2] = 30;
            bgra[i + 3] = 255;
        }
        ++framesDelivered;
        return true;
    }

    NdiSourceInfo lastRequestedSource() const {
        std::lock_guard lock(mutex_);
        return lastRequestedSource_;
    }

    // ---- send side ---------------------------------------------------------------------------
    std::atomic<bool> failSendCreate{false};
    std::atomic<int> sendCreateCalls{0}, sendDestroyCalls{0}, sendVideoCalls{0};

    void* sendCreate(const std::string& ndiName) override {
        ++sendCreateCalls;
        std::lock_guard lock(mutex_);
        lastSendName_ = ndiName;
        if (!available_.load() || failSendCreate.load()) return nullptr;
        return &sendToken_;
    }
    void sendDestroy(void* handle) override {
        if (handle == &sendToken_) ++sendDestroyCalls;
    }
    void sendVideo(void* handle, const std::uint8_t* bgra, int width, int height, int stride, int fpsNum,
                   int fpsDen) override {
        if (handle != &sendToken_) return;
        ++sendVideoCalls;
        std::lock_guard lock(mutex_);
        lastSentWidth_ = width;
        lastSentHeight_ = height;
        lastSentFpsNum_ = fpsNum;
        lastSentFpsDen_ = fpsDen;
        lastSentFirstPixel_.assign(bgra, bgra + (width > 0 ? std::min(static_cast<std::size_t>(4), static_cast<std::size_t>(stride)) : 0));
    }
    std::string lastSendName() const { std::lock_guard lock(mutex_); return lastSendName_; }
    int lastSentWidth() const { std::lock_guard lock(mutex_); return lastSentWidth_; }
    int lastSentHeight() const { std::lock_guard lock(mutex_); return lastSentHeight_; }
    std::vector<std::uint8_t> lastSentFirstPixel() const { std::lock_guard lock(mutex_); return lastSentFirstPixel_; }

private:
    mutable std::mutex mutex_;
    std::vector<NdiSourceInfo> sources_;
    int sendToken_ = 0;
    std::string lastSendName_;
    int lastSentWidth_ = 0, lastSentHeight_ = 0, lastSentFpsNum_ = 0, lastSentFpsDen_ = 0;
    std::vector<std::uint8_t> lastSentFirstPixel_;
    NdiSourceInfo lastRequestedSource_;
    int findToken_ = 0;
    int recvToken_ = 0;
};

}  // namespace hb::ndi::test
