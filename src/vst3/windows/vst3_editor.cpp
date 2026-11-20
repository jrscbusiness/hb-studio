// Windows-only: the VST3 plugin's own native GUI editor window (IVst3Editor, src/vst3/vst3.hpp).
//
// IMPORTANT / HONESTY NOTE: this file cannot be compiled or exercised anywhere in this project's
// Linux-based development/CI environment (no Windows toolchain, no real VST3 plugin with an
// editor to test against). It is written carefully, against the documented IPlugView/IPlugFrame
// contract (pluginterfaces/gui/iplugview.h) and the pattern used by Steinberg's own
// public.sdk/samples/vst-hosting/editorhost sample, but it is UNVERIFIED: only a Windows build (CI
// or a real machine) can confirm it actually works. See docs/VST3.md.
//
// Design: a single top-level HWND per effect instance, created on the calling (UI/control) thread.
// It does NOT run its own message loop — HB Studio's existing WebView2 host app already pumps
// Windows messages on that thread, and any top-level window created there is dispatched through
// that same loop, exactly like every other window the app creates.
#include <windows.h>

#include <string>

#include "pluginterfaces/base/funknownimpl.h"
#include "pluginterfaces/gui/iplugview.h"
#include "vst3/vst3_effect.hpp"

namespace Vst = Steinberg::Vst;

namespace hb::vst3 {
namespace {

const wchar_t* const kWindowClassName = L"HBStudioVst3EditorWindow";

std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    const int needed = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (needed <= 0) return {};
    std::wstring w(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), needed);
    return w;
}

// Resizes `hwnd` so its CLIENT area becomes exactly width x height, keeping its current position.
void resizeClientTo(HWND hwnd, int width, int height) {
    RECT r{0, 0, width, height};
    const auto style = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE));
    const auto exStyle = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
    AdjustWindowRectEx(&r, style, FALSE, exStyle);
    SetWindowPos(hwnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

LRESULT CALLBACK editorWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_CLOSE) {
        // The plugin keeps processing audio regardless (see vst3.hpp); only the window goes away.
        auto* self = reinterpret_cast<Vst3Effect*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (self != nullptr) self->closeEditor();  // tears this HWND down itself
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void ensureWindowClassRegistered() {
    static bool registered = false;
    if (registered) return;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &editorWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kWindowClassName;
    RegisterClassExW(&wc);
    registered = true;
}

// Minimal IPlugFrame: reacts to the plugin asking for a resize by resizing our HWND to match and
// forwarding onSize() back to the view, per the documented host contract. Lifetime is owned by
// EditorState (non-refcounted: addRef()/release() are stubs, like other minimal hosts do for a
// frame whose lifetime the host — not the plugin — controls); view->setFrame(nullptr) is called
// before this is destroyed, in closeEditor(), so the plugin never calls into a dangling frame.
class EditorFrame final : public Steinberg::IPlugFrame {
public:
    explicit EditorFrame(HWND hwnd) : hwnd_(hwnd) {}

    Steinberg::tresult PLUGIN_API resizeView(Steinberg::IPlugView* view, Steinberg::ViewRect* newSize) override {
        if (newSize == nullptr) return Steinberg::kInvalidArgument;
        resizeClientTo(hwnd_, newSize->getWidth(), newSize->getHeight());
        if (view != nullptr) view->onSize(newSize);
        return Steinberg::kResultTrue;
    }

    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid, void** obj) override {
        if (obj == nullptr) return Steinberg::kInvalidArgument;
        if (Steinberg::FUnknownPrivate::iidEqual(iid, Steinberg::IPlugFrame::iid) ||
            Steinberg::FUnknownPrivate::iidEqual(iid, Steinberg::FUnknown::iid)) {
            *obj = static_cast<Steinberg::IPlugFrame*>(this);
            return Steinberg::kResultTrue;
        }
        *obj = nullptr;
        return Steinberg::kNoInterface;
    }
    Steinberg::uint32 PLUGIN_API addRef() override { return 1; }
    Steinberg::uint32 PLUGIN_API release() override { return 1; }

private:
    HWND hwnd_;
};

}  // namespace

struct Vst3Effect::EditorState {
    HWND hwnd = nullptr;
    Steinberg::IPtr<Steinberg::IPlugView> view;
    std::unique_ptr<EditorFrame> frame;
    bool attached = false;
};

void Vst3Effect::destroyEditorState(EditorState* state) noexcept { delete state; }

bool Vst3Effect::hasEditor() const {
    if (editor_ && editor_->attached) return true;
    try {
        auto controller = controllerForEditor();
        if (!controller) return false;
        Steinberg::IPtr<Steinberg::IPlugView> probe = Steinberg::owned(controller->createView(Vst::ViewType::kEditor));
        if (!probe) return false;
        return probe->isPlatformTypeSupported(Steinberg::kPlatformTypeHWND) == Steinberg::kResultTrue;
    } catch (...) {
        return false;  // an untrusted plugin misbehaving here must never crash the host
    }
}

bool Vst3Effect::editorOpen() const { return editor_ && editor_->attached; }

bool Vst3Effect::openEditor(const std::string& title, std::string& error) {
    error.clear();
    if (editorOpen()) {
        SetForegroundWindow(editor_->hwnd);
        return true;
    }

    Steinberg::IPtr<Vst::IEditController> controller;
    Steinberg::IPtr<Steinberg::IPlugView> view;
    try {
        controller = controllerForEditor();
        if (!controller) {
            error = "o plugin não possui um IEditController";
            return false;
        }
        view = Steinberg::owned(controller->createView(Vst::ViewType::kEditor));
        if (!view) {
            error = "o plugin não implementa um editor gráfico";
            return false;
        }
        if (view->isPlatformTypeSupported(Steinberg::kPlatformTypeHWND) != Steinberg::kResultTrue) {
            error = "o editor deste plugin não suporta janelas nativas do Windows (HWND)";
            return false;
        }
    } catch (const std::exception& e) {
        error = std::string("o plugin lançou uma exceção ao criar o editor: ") + e.what();
        return false;
    } catch (...) {
        error = "o plugin lançou uma exceção desconhecida ao criar o editor";
        return false;
    }

    ensureWindowClassRegistered();
    const std::wstring wtitle = utf8ToWide(title);
    HWND hwnd = CreateWindowExW(0, kWindowClassName, wtitle.c_str(), WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX, CW_USEDEFAULT,
                                CW_USEDEFAULT, 480, 320, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (hwnd == nullptr) {
        error = "falha ao criar a janela nativa do editor";
        return false;
    }
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));

    auto state = std::unique_ptr<EditorState, decltype(&destroyEditorState)>(new EditorState(), &destroyEditorState);
    state->hwnd = hwnd;
    state->view = view;
    state->frame = std::make_unique<EditorFrame>(hwnd);

    try {
        view->setFrame(state->frame.get());
        if (view->attached(hwnd, Steinberg::kPlatformTypeHWND) != Steinberg::kResultTrue) {
            error = "IPlugView::attached() falhou";
            view->setFrame(nullptr);
            DestroyWindow(hwnd);
            return false;
        }
    } catch (const std::exception& e) {
        error = std::string("o plugin lançou uma exceção ao anexar o editor: ") + e.what();
        DestroyWindow(hwnd);
        return false;
    } catch (...) {
        error = "o plugin lançou uma exceção desconhecida ao anexar o editor";
        DestroyWindow(hwnd);
        return false;
    }

    Steinberg::ViewRect r;
    if (view->getSize(&r) == Steinberg::kResultTrue) resizeClientTo(hwnd, r.getWidth(), r.getHeight());

    state->attached = true;
    editor_ = std::move(state);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    return true;
}

void Vst3Effect::closeEditor() {
    if (!editor_) return;
    try {
        if (editor_->view) {
            editor_->view->setFrame(nullptr);
            editor_->view->removed();
        }
    } catch (...) {
        // best-effort: an untrusted plugin misbehaving on teardown must not stop us from closing
    }
    HWND hwnd = editor_->hwnd;
    editor_.reset();  // drop our references before destroying the window
    if (hwnd != nullptr) DestroyWindow(hwnd);
}

}  // namespace hb::vst3
