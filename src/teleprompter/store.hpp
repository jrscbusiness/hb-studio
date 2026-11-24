#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// Script library on disk: <dir>/<id>.txt (UTF-8 script text) + <dir>/index.tsv ("id<TAB>name" lines).
// File names are generated ASCII ids ("s1", "s2", ...) so a display name with accents/emoji never
// touches the file system (no code-page problems on Windows) and can't escape the directory.
// Not thread-safe: the owner serialises access.
namespace hb::teleprompter {

struct ScriptInfo {
    std::string id;
    std::string name;
    int words = 0;
    std::int64_t bytes = 0;
};

constexpr std::size_t kMaxScripts = 200;
constexpr std::size_t kMaxNameBytes = 80;

// Display-name rules: control characters/tabs -> space, trimmed, whitespace collapsed, truncated to
// kMaxNameBytes on a UTF-8 boundary; empty -> "Sem título".
std::string sanitizeName(const std::string& name);
// "s" followed by 1..9 digits.
bool isValidId(const std::string& id);

class ScriptStore {
public:
    explicit ScriptStore(std::filesystem::path dir) : dir_(std::move(dir)) {}

    bool ensureDir();
    std::vector<ScriptInfo> list();                       // sorted by name (case-insensitive ASCII), then id
    // Each returns the new/affected id, or "" on failure (invalid id/limit reached/IO error).
    std::string create(const std::string& name, const std::string& text = {});
    std::string importText(const std::string& name, const std::string& rawText);   // normalises encoding
    bool load(const std::string& id, std::string& textOut);
    bool save(const std::string& id, const std::string& text);
    bool rename(const std::string& id, const std::string& name);
    bool remove(const std::string& id);
    bool exists(const std::string& id);
    std::string nameOf(const std::string& id);

private:
    struct Entry { std::string id, name; };
    std::vector<Entry> readIndex();
    bool writeIndex(const std::vector<Entry>& e);
    std::filesystem::path textPath(const std::string& id) const { return dir_ / (id + ".txt"); }
    std::string uniqueName(const std::vector<Entry>& e, const std::string& wanted, const std::string& exceptId) const;
    bool writeAtomic(const std::filesystem::path& p, const std::string& data);

    std::filesystem::path dir_;
};

}  // namespace hb::teleprompter
