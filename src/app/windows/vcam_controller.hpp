#pragma once
#include <windows.h>
#include <mfidl.h>
#include <mfvirtualcamera.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "core/logger.hpp"
#include "video/program_output.hpp"
#include "virtual-camera/windows/vcam_sender.hpp"

namespace hb::win {

// Activates/deactivates the "HB Studio" Media Foundation virtual camera and
// streams the Program Output into it. Installation of the media source DLL is
// done by the installer (HKLM registration needs administrator rights).
class VirtualCameraController {
public:
    struct Status {
        bool supported = false;   // Windows 11 build 22000+
        bool installed = false;   // DLL registered
        bool active = false;
        bool connected = false;   // a camera client (OBS...) is receiving frames
        bool busy = false;
        bool error = false;
        std::string message;
        std::uint64_t sent = 0;
    };

    VirtualCameraController(video::ProgramOutput& program, core::Logger& log);
    ~VirtualCameraController();

    Status status();
    void activate();     // asynchronous
    void deactivate();   // asynchronous
    void shutdown();     // synchronous (app exit)

    static bool isInstalled();

private:
    void join();
    void doActivate();
    void doDeactivate();
    void setMessage(std::string m, bool err);

    video::ProgramOutput& program_;
    core::Logger& log_;
    std::mutex mutex_;
    std::thread worker_;
    std::atomic<bool> busy_{false};
    bool active_ = false, error_ = false;
    std::string message_;
    IMFVirtualCamera* vcam_ = nullptr;
    std::unique_ptr<hb::vcam::PipeFrameSender> sender_;
    bool mfStarted_ = false;
};

}  // namespace hb::win
