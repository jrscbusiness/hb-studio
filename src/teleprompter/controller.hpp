#pragma once
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "teleprompter/script.hpp"
#include "teleprompter/scroll.hpp"
#include "teleprompter/store.hpp"

// Host-authoritative teleprompter: owns the scroll state machine, the script library and the
// presenter style, parses the "tp|..." command family and renders the JSON the UI/presenter use.
// Contains no platform code, so the whole command surface is unit-tested on Linux.
//
// Commands (UI -> host, and the API a future remote-control agent can drive; the message string is
// "tp|<cmd>|<args>", at most 4 '|'-separated parts, the last one keeps any further '|'):
//   play | pause | toggle | restart
//   speed|<n>            value in the current mode's unit (wpm or px/s)
//   nudge|<+-n>          relative speed change in the current mode's unit
//   mode|wpm|pxs         speed unit (the effective speed is preserved)
//   seek|<0..1>          jump to a fraction of the script
//   countdown|<0..30>    seconds of countdown before the first line starts moving
//   layout|<p|o>|<contentPx>|<viewportPx>   size reported by a view (p = presenter, o = operator preview)
//   select|<id> new|<name> rename|<id>|<name> delete|<id> save|<id>|<text> import|<name>|<text>
//   style|<key>|<value>  font lh margin fg bg mirror flip marker markerpos
//   presenter|open|close|fullscreen   (acted upon by the Windows host, see Effects::presenter)
//   hello                (a view announces itself: everything is re-sent)
namespace hb::teleprompter {

struct Style {
    int fontPx = 64;        // 20..220
    double lineHeight = 1.4;  // 1.0..2.5
    int marginPct = 8;      // left/right margin, % of width, 0..30
    std::string fg = "#FFFFFF";
    std::string bg = "#000000";
    bool mirror = false;    // horizontal mirror (beam-splitter glass)
    bool flip = false;      // vertical flip
    bool marker = true;     // reading-line marker
    int markerPct = 35;     // reading line position, % from the top, 10..80
};

struct Effects {
    bool handled = false;    // the message was a "tp|..." command
    bool state = false, list = false, script = false, style = false;
    enum class Presenter { None, Open, Close, Fullscreen } presenter = Presenter::None;
    std::string toast;       // pt-BR message for the operator ("" = none)
};

class Controller {
public:
    Controller(std::filesystem::path dir, const Clock* clock);

    // Creates the folder, loads settings and the last script (creating a sample on first run).
    void load();
    // Parses and applies one UI message (any message; non "tp|" ones return handled=false).
    Effects execute(const std::string& message);
    // State JSON to broadcast now if due (state transition, or the periodic re-sync while moving).
    std::optional<std::string> poll();
    void setPresenterOpen(bool open);
    void flush();   // persists settings now

    std::string stateJson();
    std::string listJson();
    std::string scriptJson();
    std::string styleJson();

    // Test/inspection helpers.
    Snapshot snapshot();
    Style style();

    static constexpr double kResyncSeconds = 0.5;

private:
    Effects executeLocked(const std::string& message);
    bool selectLocked(const std::string& id);
    void saveSettingsLocked();
    void markSettingsDirty();
    std::string stateJsonLocked();

    mutable std::mutex mu_;
    const Clock* clock_;
    std::filesystem::path dir_;
    ScriptStore store_;
    ScrollState scroll_;
    Style style_;
    std::string currentId_, currentName_, currentText_;
    ParsedScript parsed_;
    bool presenterOpen_ = false;
    bool presenterLayout_ = false;   // a presenter view has reported its layout since it opened
    unsigned seq_ = 0;
    RunState lastSentState_ = RunState::Stopped;
    double lastSentAt_ = -1e9;
    bool settingsDirty_ = false;
    double settingsDirtyAt_ = 0;
};

}  // namespace hb::teleprompter
