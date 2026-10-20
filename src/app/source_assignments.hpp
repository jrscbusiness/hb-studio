#pragma once
#include <array>
#include <mutex>
#include <string>

namespace hb::app {

// Which input device is assigned to each of the 8 slots (CAM 1..8), persisted between runs.
// File format (one line per slot, tab separated): "<slot>\t<deviceId>\t<displayName>".
// Unreadable or hostile files are ignored line by line; ids/names are sanitised.
class SourceAssignments {
public:
    static constexpr int kSlots = 8;
    struct Entry {
        std::string id, name;
        bool empty() const { return id.empty(); }
    };

    Entry get(int slot) const;
    bool set(int slot, std::string id, std::string name);   // false: bad slot or id
    void clear(int slot);

    std::string serialize() const;
    void parse(const std::string& text);
    bool load(const std::string& path);   // false: missing file (state untouched)
    bool save(const std::string& path) const;  // atomic

    static std::string sanitize(const std::string& s, std::size_t maxLen);

private:
    mutable std::mutex mutex_;
    std::array<Entry, kSlots> e_{};
};

}  // namespace hb::app
