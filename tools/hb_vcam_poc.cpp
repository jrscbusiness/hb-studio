// hb_vcam_poc: Windows 11 virtual camera proof of concept.
//
// Creates a session-lifetime Media Foundation virtual camera backed by the
// HB Studio media source DLL (installed once with tools/vcam/install.ps1),
// then feeds it the Program Output (colour bars + moving marker, 1080p30).
//
//   hb_vcam_poc [seconds]      (0 or omitted = run until Enter is pressed)
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfvirtualcamera.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <thread>

#include "video/program_output.hpp"
#include "virtual-camera/windows/vcam_protocol.hpp"
#include "virtual-camera/windows/vcam_sender.hpp"

namespace {
const char* explain(HRESULT hr) {
    switch (static_cast<unsigned long>(hr)) {
        case 0x80070005: return "ACCESS DENIED: check Settings > Privacy & security > Camera (allow desktop apps).";
        case 0x80004002: return "E_NOINTERFACE";
        case 0x80040154: return "Media source class not registered: run tools\\vcam\\install.ps1 as Administrator.";
        case 0x80070002: return "File not found: is the DLL installed (install.ps1)?";
        case 0x80070057: return "Invalid argument.";
        default: return "";
    }
}
}  // namespace

int main(int argc, char** argv) {
    const int seconds = argc > 1 ? std::atoi(argv[1]) : 0;

    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 1;
    if (FAILED(MFStartup(MF_VERSION))) {
        std::puts("MFStartup failed");
        return 1;
    }

    IMFVirtualCamera* vcam = nullptr;
    // Windows appends "Windows Virtual Camera" to this name.
    HRESULT hr = MFCreateVirtualCamera(MFVirtualCameraType_SoftwareCameraSource,
                                       MFVirtualCameraLifetime_Session, MFVirtualCameraAccess_CurrentUser,
                                       L"HB Studio", hb::vcam::kMediaSourceClsidString, nullptr, 0, &vcam);
    if (FAILED(hr)) {
        std::printf("MFCreateVirtualCamera failed: 0x%08lX %s\n", static_cast<unsigned long>(hr), explain(hr));
        return 2;
    }
    hr = vcam->Start(nullptr);
    if (FAILED(hr)) {
        std::printf("IMFVirtualCamera::Start failed: 0x%08lX %s\n", static_cast<unsigned long>(hr), explain(hr));
        vcam->Release();
        return 3;
    }
    std::puts("Virtual camera created: look for \"HB Studio Windows Virtual Camera\" in your apps.");

    hb::video::ProgramOutput program({hb::vcam::kWidth, hb::vcam::kHeight, {hb::vcam::kFpsNum, hb::vcam::kFpsDen}});
    hb::vcam::PipeFrameSender sender(program);
    sender.start();
    program.start();

    if (seconds > 0) {
        std::printf("Running for %d s...\n", seconds);
    } else {
        std::puts("Running. Press Enter to stop.");
    }
    const auto t0 = std::chrono::steady_clock::now();
    bool lastConnected = false;
    std::thread input;
    std::atomic<bool> enter{false};
    if (seconds <= 0) input = std::thread([&] { std::cin.get(); enter = true; });
    while (!enter) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        const bool c = sender.connected();
        if (c != lastConnected) {
            std::puts(c ? "[camera client active: sending frames]" : "[camera closed/idle: waiting for a client app to open it]");
            lastConnected = c;
        }
        if (seconds > 0 && std::chrono::steady_clock::now() - t0 > std::chrono::seconds(seconds)) break;
    }
    std::printf("Frames produced: %llu, sent to camera: %llu\n",
                static_cast<unsigned long long>(program.framesProduced()),
                static_cast<unsigned long long>(sender.framesSent()));

    program.stop();
    sender.stop();
    vcam->Shutdown();
    vcam->Release();
    MFShutdown();
    CoUninitialize();
    if (input.joinable()) input.detach();
    return 0;
}
