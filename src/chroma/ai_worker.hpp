#pragma once
// Runs segmentation on its own thread so the 30 fps program clock is NEVER blocked.
//   * submit() is non-blocking: it copies a small frame into a single "pending" slot (an older
//     pending frame is overwritten and counted as dropped).
//   * the worker thread infers, post-processes (levels -> erode -> feather -> temporal EMA) and
//     publishes an immutable mask; consumers call latest() and reuse it until a newer one lands.
//   * inference time is measured (EMA) and exposed for the UI's "computador lento" warning.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "chroma/mask_ops.hpp"
#include "chroma/segmenter.hpp"
#include "chroma/types.hpp"

namespace hb::chroma {

struct PostParams {
    int levelLo = 90, levelHi = 170;   // 0..255 matte levels (threshold centre/width)
    int erodePasses = 1;
    int featherRadius = 1;
    int emaNew = 170;                  // 1..256
    int minIntervalMs = 66;            // min time between inference starts (0 = as fast as possible)
};

// Maps the operator's AI settings (tolerance = threshold centre, softness = band width) and quality
// level to concrete post-processing parameters.
PostParams makePostParams(const Settings& s);

class AiMaskWorker {
public:
    explicit AiMaskWorker(std::shared_ptr<ISegmenter> seg, PostParams p = {});
    ~AiMaskWorker();   // requests stop and detaches if stop() was not called (never blocks the caller)
    AiMaskWorker(const AiMaskWorker&) = delete;
    AiMaskWorker& operator=(const AiMaskWorker&) = delete;

    void start();
    void stop();   // joins; call from a non-realtime thread

    // True when a new frame would be accepted right now (not busy, interval elapsed, not failed).
    // Lets the caller skip the downscale entirely when the answer is no.
    bool wantsFrame() const;
    // Copies `frame` (any size; the WORKER box-downscales it to the segmenter's input size, so that
    // cost never lands on the caller's thread) as the pending frame. Never blocks on inference.
    // Returns false if it was not accepted.
    bool submit(const video::Frame& frame);

    std::shared_ptr<const Mask> latest() const;
    void setParams(const PostParams& p);

    struct Stats {
        double inferMsAvg = 0;       // EMA of segment() duration
        double inferMsLast = 0;
        std::uint64_t processed = 0, dropped = 0, failures = 0;
        bool failed = false;         // gave up after repeated errors (see message)
        std::string message;
    };
    Stats stats() const;
    int inputWidth() const;
    int inputHeight() const;

private:
    struct State;
    std::shared_ptr<State> st_;
    std::thread th_;
    bool stopped_ = false;
};

}  // namespace hb::chroma
