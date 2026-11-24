#include "app/windows/presenter_window.hpp"

namespace hb::win {

PresenterWindow::PresenterWindow(std::function<void(const std::string&)> fromPage, std::function<void(bool)> onState)
    : fromPage_(std::move(fromPage)), onState_(std::move(onState)) {}

PresenterWindow::~PresenterWindow() { close(); }

bool PresenterWindow::isOpen() const {
    std::lock_guard lock(mu_);
    return host_ && !closed_;
}

void PresenterWindow::retireClosed() {
    std::unique_ptr<WebViewHost> doomed;
    {
        std::lock_guard lock(mu_);
        if (host_ && closed_) doomed = std::move(host_);
    }
    // Destroyed outside the lock and outside the window's own message handler (we are called from
    // open()/close(), never from onClosed), since WebViewHost's destructor closes the WebView2 controller.
    doomed.reset();
}

bool PresenterWindow::open(HINSTANCE inst, const std::wstring& userDataDir, const std::wstring& placementFile,
                           std::string html, std::function<std::vector<std::string>()> initialMessages) {
    if (html.empty()) return false;
    {
        std::lock_guard lock(mu_);
        if (host_ && !closed_) {
            HWND h = host_->hwnd();
            if (h) {
                ShowWindow(h, IsIconic(h) ? SW_RESTORE : SW_SHOW);
                SetForegroundWindow(h);
            }
            return true;
        }
    }
    retireClosed();
    auto host = std::make_unique<WebViewHost>();
    WebViewHost* raw = host.get();
    WebViewHost::Options opt;
    opt.title = L"HB Studio — TELEPROMPTER";
    opt.width = 1000;
    opt.height = 700;
    opt.minWidth = 320;
    opt.minHeight = 240;
    opt.userDataDir = userDataDir;   // same profile folder as the main window: one shared browser process
    opt.background = RGB(0, 0, 0);
    opt.initialDark = true;
    opt.placementFile = placementFile;
    opt.secondary = true;
    opt.onClosed = [this] {
        {
            std::lock_guard lock(mu_);
            closed_ = true;
        }
        if (onState_) onState_(false);
    };
    {
        std::lock_guard lock(mu_);
        host_ = std::move(host);
        closed_ = false;
    }
    const bool ok = raw->create(
        inst, opt, std::move(html),
        [this](const std::string& m) { if (fromPage_) fromPage_(m); },
        [raw, initialMessages = std::move(initialMessages)] {
            if (initialMessages)
                for (auto& j : initialMessages()) raw->postQueued("", j);
        });
    if (!ok) {
        std::lock_guard lock(mu_);
        closed_ = true;
        return false;
    }
    if (onState_) onState_(true);
    return true;
}

void PresenterWindow::close() {
    HWND h = nullptr;
    {
        std::lock_guard lock(mu_);
        if (host_ && !closed_) h = host_->hwnd();
    }
    if (h) DestroyWindow(h);   // fires onClosed through WM_DESTROY
    retireClosed();
}

void PresenterWindow::toggleFullscreen() {
    std::lock_guard lock(mu_);
    if (host_ && !closed_) host_->toggleFullscreen();
}

void PresenterWindow::broadcast(const std::string& key, const std::string& json) {
    std::lock_guard lock(mu_);
    if (host_ && !closed_) host_->postQueued(key, json);
}

}  // namespace hb::win
