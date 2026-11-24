#pragma once
// Minimal file-system abstraction so the engine and the folder watcher are testable without disk.
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace hb::cloud {

struct FileStat {
    bool exists = false;
    bool isDir = false;
    std::uint64_t size = 0;
    std::int64_t mtime = 0;  // seconds since the Unix epoch
};

class IFileSystem {
public:
    virtual ~IFileSystem() = default;
    virtual FileStat stat(const std::string& pathUtf8) = 0;
    // Regular files directly inside the folder (not recursive), full UTF-8 paths. Never throws.
    virtual std::vector<std::string> listFiles(const std::string& dirUtf8) = 0;
    // Reads up to `len` bytes at `offset`. false on any error or short read before EOF.
    virtual bool read(const std::string& pathUtf8, std::uint64_t offset, std::size_t len, std::string& out) = 0;
    virtual bool remove(const std::string& pathUtf8) = 0;
    // true if another process has the file open for writing (Windows: exclusive-open probe). Best effort.
    virtual bool isLocked(const std::string&) { return false; }
};

// std::filesystem based implementation (UTF-8 paths on both Windows and POSIX).
std::shared_ptr<IFileSystem> makeStdFileSystem();

// File name part of a UTF-8 path (handles both separators).
std::string baseName(const std::string& pathUtf8);
// Lowercase extension without the dot ("mp4"), "" if none.
std::string fileExtension(const std::string& pathUtf8);

}  // namespace hb::cloud
