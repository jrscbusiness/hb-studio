#pragma once
// SceneService: the scenes on the CENAS bank (slot -> Scene), user templates, persistence
// (scenes.json) and the UI command protocol. Portable; the app only forwards messages.
//
// Commands (the first field is the command, '|' separated; payloads are JSON):
//   scenesave|<sceneJson>                 upsert (by id; new ids take the first free slot)
//   sceneapply|<templateId>|<roleMapJson>[|<sceneIdsCsv>]   roleMap: {"Pregador":0,...} (0-based slots)
//   scenedelete|<sceneId>
//   scenerename|<sceneId>|<name>
//   scenetemplate|<sceneIdsCsv>|<name>    save scene(s) as "my template"
//   templatedelete|<templateId>           user templates only
//   sceneexport|<templateId>|<path>   sceneimport|<path>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "scene/scene.hpp"

namespace hb::scene {

class SceneService {
public:
    static constexpr int kSlots = 16;
    using ApplyFn = std::function<void(int slot, std::shared_ptr<const Scene> scene)>;

    // `path` may be empty (no persistence).
    SceneService(std::string path, ApplyFn apply);

    bool load();  // false: no file (nothing changes); applies loaded scenes through ApplyFn

    struct Result {
        bool ok = true;
        bool changed = false;
        std::string message;  // pt-BR, suitable for a toast (may be empty)
    };
    bool isSceneCommand(const std::string& msg) const;
    Result handle(const std::string& msg);

    std::string stateJson() const;  // {"t":"scenes_state",...}

    std::shared_ptr<const Scene> sceneAt(int slot) const;
    std::vector<Template> userTemplates() const;

private:
    Result doSave(const std::string& payload);
    Result doApply(const std::vector<std::string>& p);
    Result doDelete(const std::string& id);
    Result doRename(const std::string& id, const std::string& name);
    Result doSaveTemplate(const std::string& ids, const std::string& name);
    Result doDeleteTemplate(const std::string& id);
    Result doExport(const std::string& id, const std::string& path);
    Result doImport(const std::string& path);
    int slotOf(const std::string& id) const;
    int freeSlots() const;
    int firstFree() const;
    std::string uniqueSceneId(const std::string& base) const;
    const Template* findTemplate(const std::string& id) const;
    void put(int slot, std::shared_ptr<const Scene> s);
    bool save() const;

    std::string path_;
    ApplyFn apply_;
    mutable std::mutex mutex_;
    std::vector<std::shared_ptr<const Scene>> slots_;
    std::vector<Template> user_;
};

}  // namespace hb::scene
