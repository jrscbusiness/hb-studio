#include "app/windows/output_window.hpp"

namespace hb::win {

namespace {
constexpr wchar_t kClass[] = L"HBStudioOutputWindow";
}  // namespace

OutputWindow::OutputWindow(std::wstring title) : title_(std::move(title)) {}

OutputWindow::~OutputWindow() { close(); }

bool OutputWindow::open(HINSTANCE inst) {
    if (hwnd_) {
        ShowWindow(hwnd_, IsIconic(hwnd_) ? SW_RESTORE : SW_SHOW);
        SetForegroundWindow(hwnd_);
        return true;
    }
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);  // benign if already registered (a second OutputWindow instance)

    hwnd_ = CreateWindowExW(0, kClass, title_.c_str(), WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 960,
                            540, nullptr, nullptr, inst, this);
    if (!hwnd_) return false;
    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);
    return true;
}

void OutputWindow::close() {
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

bool OutputWindow::isOpen() const { return hwnd_ != nullptr; }

void OutputWindow::present(const video::Frame& frame) {
    if (frame.width <= 0 || frame.height <= 0) return;
    HWND target;
    {
        std::lock_guard lock(mutex_);
        latest_ = frame;  // std::vector's copy-assign reuses the buffer once sizes line up
        haveFrame_ = true;
        target = hwnd_;
    }
    if (target) InvalidateRect(target, nullptr, FALSE);
}

void OutputWindow::toggleFullscreen() {
    const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(hwnd_, GWL_STYLE));
    if (!fullscreen_) {
        savedPlacement_.length = sizeof(savedPlacement_);
        GetWindowPlacement(hwnd_, &savedPlacement_);
        MONITORINFO mi{sizeof(mi)};
        if (GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &mi)) {
            SetWindowLongPtrW(hwnd_, GWL_STYLE, style & ~static_cast<DWORD>(WS_OVERLAPPEDWINDOW));
            SetWindowPos(hwnd_, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                        mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                        SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
            fullscreen_ = true;
        }
    } else {
        SetWindowLongPtrW(hwnd_, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(hwnd_, &savedPlacement_);
        SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
                    SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        fullscreen_ = false;
    }
}

LRESULT CALLBACK OutputWindow::wndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    OutputWindow* self = nullptr;
    if (m == WM_NCCREATE) {
        self = static_cast<OutputWindow*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = h;  // valid before CreateWindowExW returns: handle() needs it for this very message
    } else {
        self = reinterpret_cast<OutputWindow*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    }
    return self ? self->handle(m, w, l) : DefWindowProcW(h, m, w, l);
}

LRESULT OutputWindow::handle(UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_ERASEBKGND: return 1;  // painted fully every time; avoids a black flash on resize
        case WM_LBUTTONDBLCLK:
            toggleFullscreen();
            return 0;
        case WM_KEYDOWN:
            if (w == VK_F11) { toggleFullscreen(); return 0; }
            if (w == VK_ESCAPE && fullscreen_) { toggleFullscreen(); return 0; }
            break;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd_, &ps);
            RECT client;
            GetClientRect(hwnd_, &client);
            const int cw = client.right - client.left, ch = client.bottom - client.top;
            std::lock_guard lock(mutex_);
            if (haveFrame_ && cw > 0 && ch > 0) {
                // Letterbox: scale the source to fit the window, preserving aspect ratio, and
                // paint the bars black first (StretchDIBits only touches the destination rect).
                const double srcAr = static_cast<double>(latest_.width) / latest_.height;
                const double dstAr = static_cast<double>(cw) / ch;
                int dw = cw, dh = ch, dx = 0, dy = 0;
                if (srcAr > dstAr) { dh = static_cast<int>(cw / srcAr + 0.5); dy = (ch - dh) / 2; }
                else { dw = static_cast<int>(ch * srcAr + 0.5); dx = (cw - dw) / 2; }
                HBRUSH black = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
                if (dy > 0) { RECT r{0, 0, cw, dy}; FillRect(dc, &r, black); r = {0, dy + dh, cw, ch}; FillRect(dc, &r, black); }
                if (dx > 0) { RECT r{0, 0, dx, ch}; FillRect(dc, &r, black); r = {dx + dw, 0, cw, ch}; FillRect(dc, &r, black); }
                BITMAPINFO bi{};
                bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                bi.bmiHeader.biWidth = latest_.width;
                bi.bmiHeader.biHeight = -latest_.height;  // negative: top-down source, matches video::Frame
                bi.bmiHeader.biPlanes = 1;
                bi.bmiHeader.biBitCount = 32;
                bi.bmiHeader.biCompression = BI_RGB;
                SetStretchBltMode(dc, HALFTONE);
                StretchDIBits(dc, dx, dy, dw, dh, 0, 0, latest_.width, latest_.height, latest_.data.data(), &bi,
                             DIB_RGB_COLORS, SRCCOPY);
            } else {
                RECT r{0, 0, cw, ch};
                FillRect(dc, &r, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            }
            EndPaint(hwnd_, &ps);
            return 0;
        }
        case WM_CLOSE:
            DestroyWindow(hwnd_);
            return 0;
        case WM_DESTROY:
            // A secondary window: never PostQuitMessage here, the main window owns the message
            // loop's lifetime. hwnd_ is cleared so isOpen()/present() know it's gone; a later
            // open() just creates a fresh one.
            hwnd_ = nullptr;
            return 0;
        default: break;
    }
    return DefWindowProcW(hwnd_, m, w, l);
}

}  // namespace hb::win
