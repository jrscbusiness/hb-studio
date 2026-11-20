#pragma once
#include <windows.h>

#include <mutex>
#include <string>

#include "video/program_output.hpp"

namespace hb::win {

// A plain, lightweight native window that always shows the newest frame it's given (the PROGRAM
// or the PREVIEW), meant to be dragged by the operator to a second/third monitor and maximized or
// made borderless-fullscreen there — HB Studio's answer to "I have 2-3 monitors plugged in,
// I want PROGRAM/PREVIEW on their own screen". No WebView2, no UI chrome: just the picture,
// letterboxed to fit the window, on the same UI thread/message loop as the main window (so no
// extra thread is needed — the existing message pump already dispatches to any window the thread
// owns by HWND).
class OutputWindow {
public:
    explicit OutputWindow(std::wstring title);
    ~OutputWindow();
    OutputWindow(const OutputWindow&) = delete;
    OutputWindow& operator=(const OutputWindow&) = delete;

    // Creates the window if it isn't open yet (idempotent) and brings it to the foreground either
    // way. False only if window creation itself failed.
    bool open(HINSTANCE inst);
    // Destroys the window; safe to call even when not open.
    void close();
    bool isOpen() const;

    // Thread-safe; call as often as useful (every Program/Preview tick). Copies the frame (reusing
    // its own buffer when the size matches) and schedules a repaint; never blocks on GDI.
    void present(const video::Frame& frame);

private:
    static LRESULT CALLBACK wndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT handle(UINT, WPARAM, LPARAM);
    void toggleFullscreen();

    std::wstring title_;
    HWND hwnd_ = nullptr;
    bool fullscreen_ = false;
    WINDOWPLACEMENT savedPlacement_{sizeof(WINDOWPLACEMENT)};

    mutable std::mutex mutex_;
    video::Frame latest_;
    bool haveFrame_ = false;
};

}  // namespace hb::win
