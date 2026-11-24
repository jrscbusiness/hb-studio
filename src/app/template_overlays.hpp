#pragma once
#include <string>

#include "core/json.hpp"
#include "overlay/manager.hpp"
#include "scene/scene.hpp"

namespace hb::app {

// Scene templates carry overlay presets as plain data ({"type":"lowerthird","text":"{Palestrante}",...}).
// This turns them into real, OFF-AIR overlay elements the operator can edit and put on air from the
// GRÁFICOS tab. Elements get stable ids ("tpl-<sceneId>-<n>") and are only created when missing, so
// re-applying a template never clobbers edits the operator already made. Returns how many were created.
inline int createOverlaysFromScene(overlay::OverlayManager& mgr, const scene::Scene& scene) {
    // "{Palestrante}" -> "Palestrante": the role/placeholder name stays as editable text instead of
    // showing literal braces on the broadcast.
    auto unbrace = [](std::string s) {
        std::string out;
        for (char c : s)
            if (c != '{' && c != '}') out += c;
        return out;
    };
    auto str = [](const core::json::Value& v, const char* key) {
        const auto* f = v.get(key);
        return (f && f->isStr()) ? f->s : std::string();
    };
    int created = 0, n = 0;
    for (const auto& preset : scene.overlays) {
        ++n;
        if (!preset.isObj()) continue;
        const std::string type = str(preset, "type");
        if (!overlay::isValidType(type)) continue;
        const std::string id = "tpl-" + scene.id + "-" + std::to_string(n);
        if (mgr.element(id)) continue;
        overlay::ElementConfig cfg = overlay::defaultConfig(type);
        cfg.id = id;
        cfg.name = "Modelo · " + scene.name;
        if (type == "lowerthird" || type == "chatpin") {
            const std::string t = str(preset, "text");
            if (!t.empty()) cfg.title = unbrace(t);
            const std::string sub = str(preset, "subtitle");
            if (!sub.empty()) cfg.subtitle = unbrace(sub);
        } else if (type == "qr") {
            const std::string t = str(preset, "text");
            if (!t.empty()) cfg.qrText = unbrace(t);
            const std::string cap = str(preset, "caption");
            if (!cap.empty()) cfg.qrCaption = cap;
        } else if (type == "timer") {
            const std::string label = str(preset, "label");
            if (!label.empty()) cfg.label = label;
        }
        if (!mgr.upsert(cfg).empty()) ++created;
    }
    return created;
}

}  // namespace hb::app
