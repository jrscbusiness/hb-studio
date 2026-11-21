#pragma once
#include <windows.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "video/source.hpp"

namespace hb::win {

struct VideoDevice {
    std::string id;     // stable id (device path, or name for devices without one)
    std::string name;   // friendly name, e.g. "OBS Virtual Camera"
    bool virtualCam = false;   // heuristic: name looks like a software camera
};

// Lists every DirectShow video capture device: physical webcams/capture cards AND software
// cameras such as "OBS Virtual Camera" (these are DirectShow filters that Media Foundation does
// not enumerate). HB Studio's own virtual camera is excluded to avoid a video feedback loop.
// May block for a while on misbehaving drivers: call it from a worker thread.
std::vector<VideoDevice> enumerateVideoDevices();

// A capture resolution/frame rate; all-zero means "automatic" (let the device negotiate its own
// default, exactly like DShowSource has always done).
struct VideoFormat {
    int width = 0, height = 0, fpsNum = 0, fpsDen = 0;
    bool empty() const { return width <= 0 || height <= 0; }
};

// Lists the resolutions/frame rates `id` (as returned by enumerateVideoDevices) advertises via
// IAMStreamConfig, deduplicated. Empty if the device can't be probed (busy, or doesn't expose
// IAMStreamConfig) - callers must fall back to automatic in that case. May block: call off the UI
// thread, same caveat as enumerateVideoDevices.
std::vector<VideoFormat> enumerateDeviceFormats(const std::string& id);

// Captures one device with DirectShow (source -> SampleGrabber(RGB32) -> NullRenderer) on its own
// thread. All failures end up in status(); nothing throws. The newest frame is scaled (letterbox) to
// whatever size render() is asked for.
class DShowSource final : public video::LiveSource {
public:
    // `requested`: a format to ask the device for via IAMStreamConfig::SetFormat before the graph
    // is built; empty (the default) keeps today's behaviour of letting the device auto-negotiate.
    // If the device doesn't support the exact requested format, start() falls back to automatic
    // rather than failing the whole source.
    explicit DShowSource(std::string deviceId, std::string displayName, VideoFormat requested = {});
    ~DShowSource() override;
    DShowSource(const DShowSource&) = delete;
    DShowSource& operator=(const DShowSource&) = delete;

    void start();
    void stop();

    const std::string& name() const override { return name_; }
    void render(video::Frame& frame, std::uint64_t index) const override;
    video::SourceStatus status() const override;

    // Called by the grabber callback (streaming thread).
    void onBuffer(const std::uint8_t* data, long length);

private:
    struct Impl;
    void run();

    std::string id_, name_;
    VideoFormat requested_;
    std::thread thread_;
    HANDLE stopEvent_ = nullptr;

    mutable std::mutex mutex_;
    std::shared_ptr<const video::Frame> latest_;
    std::chrono::steady_clock::time_point lastFrame_{};
    std::chrono::steady_clock::time_point runningSince_{};
    bool running_ = false;      // graph is running
    bool failed_ = false;
    std::string error_;
    std::uint64_t frames_ = 0;
    std::uint64_t fpsFrames_ = 0;
    std::chrono::steady_clock::time_point fpsT0_{};
    double fps_ = 0;
    int width_ = 0, height_ = 0;
    bool bottomUp_ = true;
};

}  // namespace hb::win
