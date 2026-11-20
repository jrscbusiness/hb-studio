#include "plugins/shared_library.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace hb::plugins {

#ifdef _WIN32
namespace {
std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}
}  // namespace

bool SharedLibrary::load(const std::string& path, std::string& error) {
    if (handle_) return true;
    // Full path + restricted search: dependencies come from the plugin's own folder and System32 only.
    HMODULE m = LoadLibraryExW(widen(path).c_str(), nullptr,
                               LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!m) {
        error = "não foi possível carregar a biblioteca (erro " + std::to_string(GetLastError()) + ")";
        return false;
    }
    handle_ = m;
    return true;
}

void* SharedLibrary::symbol(const char* name) const {
    return handle_ ? reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(handle_), name)) : nullptr;
}
#else
bool SharedLibrary::load(const std::string& path, std::string& error) {
    if (handle_) return true;
    handle_ = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle_) {
        const char* e = dlerror();
        error = std::string("não foi possível carregar a biblioteca: ") + (e ? e : "erro desconhecido");
        return false;
    }
    return true;
}

void* SharedLibrary::symbol(const char* name) const { return handle_ ? dlsym(handle_, name) : nullptr; }
#endif

}  // namespace hb::plugins
