#pragma once
#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

#include "video/program_output.hpp"

namespace hb::vcam {

// Consumer of the Program Output that streams every frame to the virtual
// camera media source over the named pipe. Reconnects automatically whenever
// the camera is closed/opened by a client app (the Frame Server only loads
// the source while someone is using the camera).
//
// The Program Output calls our consumer on its clock thread, so the callback
// only stores the newest frame; a separate thread does the blocking I/O. A
// slow or absent camera therefore never delays the Program Output.
class PipeFrameSender {
public:
    explicit PipeFrameSender(video::ProgramOutput& program) : program_(program) {}
    ~PipeFrameSender() { stop(); }

    void start();
    void stop();

    bool connected() const { return connected_.load(); }
    std::uint64_t framesSent() const { return sent_.load(); }

private:
    void run();

    video::ProgramOutput& program_;
    video::ProgramOutput::ConsumerId consumerId_ = 0;
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::shared_ptr<const video::Frame> latest_;
    std::uint64_t latestSeq_ = 0;
    bool running_ = false;
    std::atomic<bool> connected_{false};
    std::atomic<std::uint64_t> sent_{0};
};

}  // namespace hb::vcam
