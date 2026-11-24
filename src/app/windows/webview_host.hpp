#pragma once
#include <windows.h>

#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <wrl.h>

#include "WebView2.h"

namespace hb::win {

// One top-level window hosting a WebView2 control that renders an HTML UI
// (loaded from memory). The host is the only bridge between UI and engine:
//   UI -> host : chrome.webview.postMessage("cmd|arg|arg")   (onMessage)
//   host -> UI : post(json)                                  (window.__hbMessage / 'message' event)
class WebViewHost {
public:
    struct Options {
        std::wstring title = L"HB Studio";
        int width = 1500, height = 900;
        int minWidth = 1100, minHeight = 680;
        bool resizable = true;
        std::wstring userDataDir;
        COLORREF background = RGB(0xF2, 0xF6, 0xFD);
        int iconResource = 1;
        // Remembers the window's size/position/maximized state across launches. Empty = don't
        // persist (always centered at width x height). Best-effort: a missing/corrupt/stale file
        // (e.g. the saved spot is off every connected monitor) is ignored and the default is used.
        std::wstring placementFile;
        // Native window chrome (title bar) at first paint: dark caption/text + DWM immersive dark
        // mode. The web content's own colour scheme is independent (CSS); this only keeps the
        // frame Windows draws around it consistent with whatever the page is about to show.
        bool initialDark = false;
        // ---- FEATURE: teleprompter ---- secondary windows (e.g. the presenter pop-out) must not end
        // the process when closed: no PostQuitMessage, and onClosed (UI thread) tells the owner.
        bool secondary = false;
        std::function<void()> onClosed;
        // ---- /FEATURE ----
    };
    using MessageHandler = std::function<void(const std::string&)>;

    ~WebViewHost();
    bool create(HINSTANCE inst, const Options& opt, std::string html, MessageHandler onMessage,
                std::function<void()> onReady);

    // UI thread only.
    void post(const std::string& json);
    // Any thread. A non-empty key replaces a still-queued message with the same key
    // (frames are dropped, not piled up, if the UI is slow).
    void postQueued(const std::string& key, std::string json);

    // Saves a PNG of the web content, then calls done on the UI thread.
    void screenshot(const std::wstring& path, std::function<void(bool)> done);

    // Switches the native window chrome (caption colour/text + DWM immersive dark mode) to match.
    // UI thread only; safe to call any time after create().
    void setTheme(bool dark);

    int run();
    void close();
    HWND hwnd() const { return hwnd_; }
    // ---- FEATURE: teleprompter ----
    // Toggles borderless fullscreen of the native window (UI thread). Same path the page's own
    // Fullscreen API request takes, usable from the host (remote command, double click, F11).
    void toggleFullscreen() { setWindowFullscreen(!fullscreen_); }
    // ---- /FEATURE ----

private:
    static LRESULT CALLBACK wndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT handle(UINT, WPARAM, LPARAM);
    void initWebView();
    void drain();

    // The web page's Fullscreen API (used for the PROGRAM monitor) only changes the WebView2
    // content itself; WebView2 requires the host to separately borderless/maximize the real
    // window, via this event. Mirrors OutputWindow::toggleFullscreen's save/restore approach.
    void setWindowFullscreen(bool enter);

    HWND hwnd_ = nullptr;
    HBRUSH bgBrush_ = nullptr;
    Options opt_;
    std::string html_;
    MessageHandler onMessage_;
    std::function<void()> onReady_;
    Microsoft::WRL::ComPtr<ICoreWebView2Controller> controller_;
    Microsoft::WRL::ComPtr<ICoreWebView2> webview_;
    bool ready_ = false;
    bool fullscreen_ = false;
    WINDOWPLACEMENT savedPlacement_{sizeof(WINDOWPLACEMENT)};

    std::mutex qMutex_;
    std::vector<std::pair<std::string, std::string>> queue_;
};

}  // namespace hb::win
