#pragma once
// Named presets and per-camera PTZ settings, persisted as simple JSON (own minimal
// serializer, no dependencies). Schema-versioned; tolerant of corrupt files.
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "ptz/camera_config.hpp"
#include "ptz/types.hpp"

namespace hb::ptz {

constexpr int kPresetSchemaVersion = 1;

struct PresetEntry {
    int id = 0;  // preset number as sent on the wire (config.deviceId(button))
    std::string name;
    std::optional<PtzPosition> position;  // required for Software storage
    bool operator==(const PresetEntry& o) const {
        return id == o.id && name == o.name && position.has_value() == o.position.has_value() &&
               (!position || (position->pan == o.position->pan && position->tilt == o.position->tilt &&
                              position->zoom == o.position->zoom && position->units == o.position->units));
    }
};

enum class LoadStatus {
    Ok,
    Missing,            // file does not exist (empty store, not an error)
    Corrupt,            // unparsable / wrong shape: store left empty, file preserved as *.corrupt
    UnsupportedVersion  // written by a newer schema: store left empty, file left untouched
};

class PresetStore {
public:
    // ---- per-camera settings ----
    CameraPtzConfig config(const std::string& cameraId) const;  // default config if unknown
    void setConfig(CameraPtzConfig cfg);                       // normalizes
    bool hasConfig(const std::string& cameraId) const;

    // ---- presets ----
    void set(const std::string& cameraId, PresetEntry entry);  // replaces entry with same id
    bool remove(const std::string& cameraId, int id);
    std::optional<PresetEntry> find(const std::string& cameraId, int id) const;
    std::vector<PresetEntry> list(const std::string& cameraId) const;  // sorted by id
    std::vector<std::string> cameras() const;
    void clear() { cams_.clear(); }

    // ---- persistence ----
    std::string toJson() const;
    LoadStatus fromJson(const std::string& text);  // never throws
    bool saveToFile(const std::string& path) const;  // atomic (temp + rename)
    LoadStatus loadFromFile(const std::string& path);
    std::size_t skippedEntries() const { return skipped_; }  // invalid entries ignored by last load

private:
    struct Record {
        CameraPtzConfig config;
        bool hasConfig = false;
        std::map<int, PresetEntry> presets;
    };
    std::map<std::string, Record> cams_;
    std::size_t skipped_ = 0;
};

}  // namespace hb::ptz
