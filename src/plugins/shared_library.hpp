#pragma once
#include <string>

namespace hb::plugins {

// dlopen/LoadLibrary wrapper. A loaded plugin library is intentionally NEVER unloaded while the
// process runs (plugin threads may still be alive; unloading code that is executing is a crash).
class SharedLibrary {
public:
    SharedLibrary() = default;
    SharedLibrary(const SharedLibrary&) = delete;
    SharedLibrary& operator=(const SharedLibrary&) = delete;

    bool load(const std::string& pathUtf8, std::string& error);
    void* symbol(const char* name) const;
    bool loaded() const { return handle_ != nullptr; }

private:
    void* handle_ = nullptr;
};

}  // namespace hb::plugins
