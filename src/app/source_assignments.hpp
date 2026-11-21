#pragma once
#include <array>
#include <mutex>
#include <string>

namespace hb::app {

// Which input device is assigned to each of the 16 slots (CAM 1..16, 2 banks of 8), persisted
// between runs. File format (one line per slot, tab separated):
//   "<slot>\t<deviceId>\t<displayName>\t<enabled 0|1>\t<w>\t<h>\t<fpsNum>\t<fpsDen>"
// The last 5 fields are optional (a file written by an older build has only the first 3): missing
// or unparseable trailing fields default to "enabled, automatic format" - 0 for w/h/fps means
// "let the device negotiate its own default", exactly like today's behaviour. Unreadable or
// hostile files are ignored line by line; ids/names are sanitised; anything past the 8th field is
// ignored (earlier builds instead merged a stray extra field into the name - that behaviour is
// superseded by this richer format, not preserved).
class SourceAssignments {
public:
    static constexpr int kSlots = 16;
    struct Entry {
        std::string id, name;
        bool enabled = true;
        int w = 0, h = 0, fpsNum = 0, fpsDen = 0;   // 0 = automatic/unspecified
        bool empty() const { return id.empty(); }
    };

    Entry get(int slot) const;
    bool set(int slot, std::string id, std::string name);   // false: bad slot or id; resets enabled/format
    void clear(int slot);
    // Turn a slot's assigned device on/off without forgetting which device it is. False if the
    // slot is out of range or has no device assigned.
    bool setEnabled(int slot, bool enabled);
    // Remembers a chosen capture format for the slot's device (0 = automatic/unspecified, same as
    // a fresh assignment). False if the slot is out of range or has no device assigned.
    bool setFormat(int slot, int w, int h, int fpsNum, int fpsDen);

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
