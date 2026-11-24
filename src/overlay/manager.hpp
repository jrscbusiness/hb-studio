#pragma once
// OverlayManager: owns the overlay elements (timer, scoreboard, QR, lower third, captions, chat pin),
// their LayerStack, JSON persistence, the UI command protocol and the EventBus feeds
// ("caption.text", "overlay.chatPin"). See docs/OVERLAYS.md.
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/event_bus.hpp"
#include "overlay/element.hpp"

namespace hb::overlay {

class OverlayManager {
public:
    using NowFn = std::function<std::int64_t()>;  // monotonic milliseconds
    static constexpr std::size_t kMaxElements = 32;

    // `bus` may be null (no feeds). `now` defaults to the steady clock. `text` defaults to the built-in
    // bitmap font (replace it with the platform rasterizer through setTextRasterizer()).
    explicit OverlayManager(core::EventBus* bus = nullptr, NowFn now = nullptr, std::shared_ptr<ITextRasterizer> text = nullptr);
    ~OverlayManager();
    OverlayManager(const OverlayManager&) = delete;
    OverlayManager& operator=(const OverlayManager&) = delete;

    void setTextRasterizer(std::shared_ptr<ITextRasterizer> text);
    std::shared_ptr<LayerStack> stack() const { return shared_->stack; }

    // Called by the Program renderer after the switcher drew the frame. The returned callable only keeps
    // the shared render state alive (not the manager), so it stays valid while the engine outlives us.
    std::function<void(video::Frame&, std::uint64_t)> programHook() const;
    // Draws the overlays on a Program-sized preview frame when the option is enabled (no-op otherwise).
    void renderPreview(video::Frame& frame) const;
    bool previewOverlay() const { return shared_->previewOverlay.load(); }
    void setPreviewOverlay(bool on);

    // ---- element management (thread-safe) ----
    // Creates (empty id) or updates an element; returns its id ("" on failure/limit).
    std::string upsert(ElementConfig cfg);
    bool remove(const std::string& id);
    bool setOn(const std::string& id, bool on);
    bool timerCommand(const std::string& id, const std::string& cmd, double arg);
    bool scoreCommand(const std::string& id, const std::string& who, int delta);
    bool move(const std::string& id, int direction);  // -1 = back (down), +1 = front (up)
    std::vector<std::string> ids() const;
    std::shared_ptr<Element> element(const std::string& id) const;

    // UI protocol (raw "cmd|a|b|..." string as posted by the page). Returns true if the command belongs
    // to the overlay module ("ov..."), false otherwise. Never throws.
    //   ovnew|<type>            ovsave|<json>        ovdel|<id>        ovshow|<id>|1|0
    //   ovtimer|<id>|start|pause|reset|toggle|set|add[|seconds]        ovscore|<id>|a|b|reset|swap[|delta]
    //   ovperiod|<id>|<text>    ovmove|<id>|up|down  ovpreview|1|0
    bool handleCommand(const std::string& raw);

    // Host -> UI message {"t":"overlay_state", ...}: config + live data of every element.
    std::string stateJson() const;

    // ---- persistence (%LOCALAPPDATA%\HBStudio\overlays.json) ----
    std::string configJson() const;
    bool loadJsonText(const std::string& text);
    bool load(const std::string& path);
    bool save(const std::string& path) const;
    // True once after any configuration change (the app saves then).
    bool takeDirty() { return dirty_.exchange(false); }

private:
    struct Shared {
        std::shared_ptr<LayerStack> stack = std::make_shared<LayerStack>();
        mutable std::mutex mutex;
        std::shared_ptr<ITextRasterizer> text;
        NowFn now;
        std::atomic<bool> previewOverlay{false};
        void draw(video::Frame& f, bool preview) const;
    };
    std::int64_t now() const { return shared_->now(); }
    void onCaption(const core::Event& e);
    void onChatPin(const core::Event& e);
    std::vector<std::shared_ptr<Element>> elementsSnapshot() const;
    void checkQr(const ElementConfig& c);

    core::EventBus* bus_;
    std::shared_ptr<Shared> shared_;
    core::SubscriptionId capSub_ = 0, chatSub_ = 0;
    mutable std::mutex mutex_;
    std::map<std::string, std::shared_ptr<Element>> elements_;
    std::map<std::string, bool> qrOk_;
    int nextId_ = 1;
    std::atomic<bool> dirty_{false};
};

}  // namespace hb::overlay
