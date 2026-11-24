#pragma once
#include <windows.h>

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "app/windows/webview_host.hpp"

namespace hb::win {

// Teleprompter presenter pop-out: a second top-level window with its own WebView2 control (same
// WebViewHost class as the main window, flagged `secondary` so closing it never quits the app),
// shown on the presenter's monitor. It runs on the main UI thread / message loop like the other
// windows; only broadcast() may be called from other threads.
class PresenterWindow {
public:
    // fromPage(msg): messages the presenter page posts (UI thread). onState(open): window opened/closed.
    PresenterWindow(std::function<void(const std::string&)> fromPage, std::function<void(bool)> onState);
    ~PresenterWindow();
    PresenterWindow(const PresenterWindow&) = delete;
    PresenterWindow& operator=(const PresenterWindow&) = delete;

    // UI thread. Creates the window (idempotent: re-focuses it if already open). `html` is the page,
    // `initialMessages` are posted once the page has loaded (script/style/state).
    bool open(HINSTANCE inst, const std::wstring& userDataDir, const std::wstring& placementFile, std::string html,
              std::function<std::vector<std::string>()> initialMessages);
    void close();                 // UI thread
    void toggleFullscreen();      // UI thread
    bool isOpen() const;
    // Any thread. No-op while closed. Keyed messages replace a still-queued one with the same key.
    void broadcast(const std::string& key, const std::string& json);

private:
    void retireClosed();   // UI thread: destroys a host whose window the user already closed

    std::function<void(const std::string&)> fromPage_;
    std::function<void(bool)> onState_;
    mutable std::mutex mu_;
    std::unique_ptr<WebViewHost> host_;   // guarded by mu_ for broadcast()
    bool closed_ = true;                  // set from WM_DESTROY (UI thread), read under mu_
};

}  // namespace hb::win
