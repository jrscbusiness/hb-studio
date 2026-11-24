#include "app/windows/webview_host.hpp"

#include <shellapi.h>
#include <shlwapi.h>
#include <dwmapi.h>
#include <cstdio>
#include <cwchar>

#include "app/windows/win_util.hpp"

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace hb::win {

namespace {
constexpr UINT WM_HB_DRAIN = WM_APP + 1;
constexpr wchar_t kClass[] = L"HBStudioHostWindow";

// Best-effort window placement persistence (size, position, maximized), one line of 5 decimal
// numbers ("left top right bottom maximized"), in workspace coordinates like WINDOWPLACEMENT.
// Never throws; a missing/corrupt file just means "no saved placement".
bool loadPlacement(const std::wstring& path, RECT& rc, bool& maximized) {
    if (path.empty()) return false;
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    char buf[160] = {0};
    const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = 0;
    int mx = 0;
    if (sscanf_s(buf, "%ld %ld %ld %ld %d", &rc.left, &rc.top, &rc.right, &rc.bottom, &mx) != 5) return false;
    maximized = mx != 0;
    return rc.right > rc.left && rc.bottom > rc.top;
}

void savePlacement(const std::wstring& path, const RECT& rc, bool maximized) {
    if (path.empty()) return;
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return;
    char buf[160];
    const int n = sprintf_s(buf, "%ld %ld %ld %ld %d", rc.left, rc.top, rc.right, rc.bottom, maximized ? 1 : 0);
    if (n > 0) fwrite(buf, 1, static_cast<size_t>(n), f);
    fclose(f);
}

// True when any part of `rc` lands on a connected monitor (a saved spot from a since-unplugged
// second monitor, or a since-shrunk desktop, must not put the window somewhere unreachable).
bool onscreen(const RECT& rc) {
    return MonitorFromRect(&rc, MONITOR_DEFAULTTONULL) != nullptr;
}

void showRuntimeMissing(HWND owner) {
    const int r = MessageBoxW(owner,
        L"O HB Studio precisa do Microsoft Edge WebView2 Runtime, que já vem com o Windows 11.\n\n"
        L"Clique em OK para abrir a página de download.",
        L"HB Studio", MB_OKCANCEL | MB_ICONINFORMATION);
    if (r == IDOK) ShellExecuteW(nullptr, L"open", L"https://go.microsoft.com/fwlink/p/?LinkId=2124703", nullptr, nullptr, SW_SHOW);
}
}  // namespace

WebViewHost::~WebViewHost() {
    if (controller_) controller_->Close();
    if (bgBrush_) DeleteObject(bgBrush_);
}

bool WebViewHost::create(HINSTANCE inst, const Options& opt, std::string html, MessageHandler onMessage,
                         std::function<void()> onReady) {
    opt_ = opt;
    html_ = std::move(html);
    onMessage_ = std::move(onMessage);
    onReady_ = std::move(onReady);

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(opt.background);
    wc.lpszClassName = kClass;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(opt.iconResource));
    wc.hIconSm = wc.hIcon;
    RegisterClassExW(&wc);

    DWORD style = opt.resizable ? WS_OVERLAPPEDWINDOW : (WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX);
    RECT saved{};
    bool savedMaximized = false;
    int x, y, w, h;
    if (loadPlacement(opt.placementFile, saved, savedMaximized) && onscreen(saved) &&
        saved.right - saved.left >= opt.minWidth && saved.bottom - saved.top >= opt.minHeight) {
        x = saved.left;
        y = saved.top;
        w = saved.right - saved.left;
        h = saved.bottom - saved.top;
    } else {
        savedMaximized = false;
        RECT r{0, 0, opt.width, opt.height};
        AdjustWindowRectEx(&r, style, FALSE, 0);
        w = r.right - r.left;
        h = r.bottom - r.top;
        x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
        y = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;
    }
    hwnd_ = CreateWindowExW(0, kClass, opt.title.c_str(), style, x < 0 ? 0 : x, y < 0 ? 0 : y, w, h, nullptr,
                            nullptr, inst, this);
    if (!hwnd_) return false;

    setTheme(opt.initialDark);  // caption bar + class background matching the UI (Windows 11; ignored elsewhere)

    ShowWindow(hwnd_, savedMaximized ? SW_SHOWMAXIMIZED : SW_SHOW);
    UpdateWindow(hwnd_);
    trace("window created");
    initWebView();
    return true;
}

void WebViewHost::initWebView() {
    std::wstring dir = opt_.userDataDir;
    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, dir.empty() ? nullptr : dir.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [this](HRESULT res, ICoreWebView2Environment* env) -> HRESULT {
                trace(env ? "environment ready" : "environment FAILED");
                if (FAILED(res) || !env) {
                    showRuntimeMissing(hwnd_);
                    PostMessageW(hwnd_, WM_CLOSE, 0, 0);
                    return S_OK;
                }
                env->CreateCoreWebView2Controller(
                    hwnd_, Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [this](HRESULT res2, ICoreWebView2Controller* ctl) -> HRESULT {
                            if (FAILED(res2) || !ctl) {
                                showRuntimeMissing(hwnd_);
                                PostMessageW(hwnd_, WM_CLOSE, 0, 0);
                                return S_OK;
                            }
                            trace("controller ready");
                            controller_ = ctl;
                            controller_->get_CoreWebView2(&webview_);
                            RECT rc;
                            GetClientRect(hwnd_, &rc);
                            controller_->put_Bounds(rc);

                            ComPtr<ICoreWebView2Settings> st;
                            webview_->get_Settings(&st);
                            st->put_AreDefaultContextMenusEnabled(FALSE);
                            st->put_IsZoomControlEnabled(FALSE);
                            st->put_IsStatusBarEnabled(FALSE);

                            EventRegistrationToken tok;
                            // The UI is a closed local page: no external navigation, no popups, so a remote
                            // page can never reach the host command bridge.
                            webview_->add_NavigationStarting(
                                Callback<ICoreWebView2NavigationStartingEventHandler>(
                                    [](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* a) -> HRESULT {
                                        LPWSTR u = nullptr;
                                        bool allow = false;
                                        if (SUCCEEDED(a->get_Uri(&u)) && u) {
                                            allow = wcsncmp(u, L"data:", 5) == 0 || wcscmp(u, L"about:blank") == 0;
                                            CoTaskMemFree(u);
                                        }
                                        if (!allow) a->put_Cancel(TRUE);
                                        return S_OK;
                                    }).Get(), &tok);
                            webview_->add_NewWindowRequested(
                                Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                                    [](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* a) -> HRESULT {
                                        a->put_Handled(TRUE);
                                        return S_OK;
                                    }).Get(), &tok);
                            webview_->add_WebMessageReceived(
                                Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                    [this](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* a) -> HRESULT {
                                        trace("web message from UI");
                                        LPWSTR w = nullptr;
                                        if (SUCCEEDED(a->TryGetWebMessageAsString(&w)) && w) {
                                            try {
                                                if (onMessage_) onMessage_(narrow(w));
                                            } catch (...) {  // a bad command must never take the UI down
                                            }
                                            CoTaskMemFree(w);
                                        }
                                        return S_OK;
                                    }).Get(), &tok);
                            webview_->add_ContainsFullScreenElementChanged(
                                Callback<ICoreWebView2ContainsFullScreenElementChangedEventHandler>(
                                    [this](ICoreWebView2* sender, IUnknown*) -> HRESULT {
                                        BOOL isFs = FALSE;
                                        sender->get_ContainsFullScreenElement(&isFs);
                                        setWindowFullscreen(isFs != FALSE);
                                        return S_OK;
                                    }).Get(), &tok);
                            webview_->add_NavigationCompleted(
                                Callback<ICoreWebView2NavigationCompletedEventHandler>(
                                    [this](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs*) -> HRESULT {
                                        trace("navigation completed");
                                        const bool first = !ready_;
                                        ready_ = true;
                                        if (first && onReady_) onReady_();   // once, not on every reload
                                        drain();
                                        return S_OK;
                                    }).Get(), &tok);
                            webview_->NavigateToString(widen(html_).c_str());
                            return S_OK;
                        }).Get());
                return S_OK;
            }).Get());
    if (FAILED(hr)) {
        showRuntimeMissing(hwnd_);
        PostMessageW(hwnd_, WM_CLOSE, 0, 0);
    }
}

void WebViewHost::post(const std::string& json) {
    if (webview_ && ready_) webview_->PostWebMessageAsJson(widen(json).c_str());
}

void WebViewHost::postQueued(const std::string& key, std::string json) {
    {
        std::lock_guard lock(qMutex_);
        bool replaced = false;
        if (!key.empty())
            for (auto& q : queue_)
                if (q.first == key) { q.second = std::move(json); replaced = true; break; }
        if (!replaced) queue_.emplace_back(key, std::move(json));
    }
    if (hwnd_) PostMessageW(hwnd_, WM_HB_DRAIN, 0, 0);
}

void WebViewHost::drain() {
    std::vector<std::pair<std::string, std::string>> q;
    {
        std::lock_guard lock(qMutex_);
        if (!ready_) return;
        q.swap(queue_);
    }
    for (auto& m : q) post(m.second);
}

void WebViewHost::setWindowFullscreen(bool enter) {
    if (enter == fullscreen_ || !hwnd_) return;
    const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(hwnd_, GWL_STYLE));
    if (enter) {
        savedPlacement_.length = sizeof(savedPlacement_);
        GetWindowPlacement(hwnd_, &savedPlacement_);
        MONITORINFO mi{sizeof(mi)};
        if (!GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &mi)) return;
        SetWindowLongPtrW(hwnd_, GWL_STYLE, style & ~static_cast<DWORD>(WS_OVERLAPPEDWINDOW));
        SetWindowPos(hwnd_, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        fullscreen_ = true;
    } else {
        SetWindowLongPtrW(hwnd_, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(hwnd_, &savedPlacement_);
        SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        fullscreen_ = false;
    }
}

void WebViewHost::setTheme(bool dark) {
    const COLORREF bg = dark ? RGB(0x0A, 0x12, 0x26) : opt_.background;
    const COLORREF cap = bg, txt = dark ? RGB(0xE8, 0xEE, 0xFF) : RGB(0x0A, 0x1E, 0x47);
    BOOL immersive = dark ? TRUE : FALSE;
    if (hwnd_) {
        DwmSetWindowAttribute(hwnd_, 35 /*DWMWA_CAPTION_COLOR*/, &cap, sizeof(cap));
        DwmSetWindowAttribute(hwnd_, 36 /*DWMWA_TEXT_COLOR*/, &txt, sizeof(txt));
        DwmSetWindowAttribute(hwnd_, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &immersive, sizeof(immersive));
        HBRUSH old = bgBrush_;
        bgBrush_ = CreateSolidBrush(bg);
        SetClassLongPtrW(hwnd_, GCLP_HBRBACKGROUND, reinterpret_cast<LONG_PTR>(bgBrush_));
        if (old) DeleteObject(old);
        // Nudges Windows into repainting the non-client area (caption) immediately; otherwise the
        // new colours often only show up after the next resize/move.
        SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        InvalidateRect(hwnd_, nullptr, TRUE);
    }
}

void WebViewHost::screenshot(const std::wstring& path, std::function<void(bool)> done) {
    ComPtr<IStream> stream;
    if (!webview_ || FAILED(SHCreateStreamOnFileEx(path.c_str(), STGM_CREATE | STGM_WRITE, FILE_ATTRIBUTE_NORMAL,
                                                   TRUE, nullptr, &stream))) {
        done(false);
        return;
    }
    trace("capture requested");
    webview_->CapturePreview(COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT_PNG, stream.Get(),
                             Callback<ICoreWebView2CapturePreviewCompletedHandler>(
                                 [done, stream](HRESULT hr) -> HRESULT {
                                     trace(SUCCEEDED(hr) ? "capture done" : "capture FAILED");
                                     done(SUCCEEDED(hr));
                                     return S_OK;
                                 }).Get());
}

int WebViewHost::run() {
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}

void WebViewHost::close() {
    if (hwnd_) PostMessageW(hwnd_, WM_CLOSE, 0, 0);
}

LRESULT CALLBACK WebViewHost::wndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    WebViewHost* self = nullptr;
    if (m == WM_NCCREATE) {
        self = static_cast<WebViewHost*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = h;
    } else {
        self = reinterpret_cast<WebViewHost*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    }
    return self ? self->handle(m, w, l) : DefWindowProcW(h, m, w, l);
}

LRESULT WebViewHost::handle(UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_SIZE:
            if (controller_) {
                RECT rc;
                GetClientRect(hwnd_, &rc);
                controller_->put_Bounds(rc);
            }
            return 0;
        case WM_GETMINMAXINFO: {
            auto* mm = reinterpret_cast<MINMAXINFO*>(l);
            RECT r{0, 0, opt_.minWidth, opt_.minHeight};
            AdjustWindowRectEx(&r, static_cast<DWORD>(GetWindowLongPtrW(hwnd_, GWL_STYLE)), FALSE, 0);
            mm->ptMinTrackSize.x = r.right - r.left;
            mm->ptMinTrackSize.y = r.bottom - r.top;
            return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_HB_DRAIN: drain(); return 0;
        case WM_DESTROY:
            if (!opt_.placementFile.empty()) {
                WINDOWPLACEMENT wp{sizeof(wp)};
                if (GetWindowPlacement(hwnd_, &wp))
                    savePlacement(opt_.placementFile, wp.rcNormalPosition, wp.showCmd == SW_SHOWMAXIMIZED);
            }
            // ---- FEATURE: teleprompter ----
            if (opt_.secondary) {
                if (opt_.onClosed) opt_.onClosed();
                return 0;
            }
            // ---- /FEATURE ----
            PostQuitMessage(0);
            return 0;
        default: break;
    }
    return DefWindowProcW(hwnd_, m, w, l);
}

}  // namespace hb::win
