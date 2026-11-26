// Windows platform pieces of the cloud module. NOT VALIDATED: written without a Windows compiler;
// Windows CI / a manual test on Windows 11 is the real verifier (see docs/BACKUP_NUVEM.md).
#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "cloud/cloud_platform.hpp"
#include "cloud/net_adapter.hpp"

namespace hb::cloud {
namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// std::filesystem + "is another process writing this file?" probe: opening with no sharing fails
// with a sharing violation while a recorder still holds the file open.
class WinFs : public IFileSystem {
public:
    WinFs() : base_(makeStdFileSystem()) {}
    FileStat stat(const std::string& p) override { return base_->stat(p); }
    std::vector<std::string> listFiles(const std::string& d) override { return base_->listFiles(d); }
    bool read(const std::string& p, std::uint64_t o, std::size_t l, std::string& out) override { return base_->read(p, o, l, out); }
    bool remove(const std::string& p) override { return base_->remove(p); }
    bool isLocked(const std::string& p) override {
        HANDLE h = ::CreateFileW(widen(p).c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return ::GetLastError() == ERROR_SHARING_VIOLATION;
        ::CloseHandle(h);
        return false;
    }

private:
    std::shared_ptr<IFileSystem> base_;
};

}  // namespace

std::shared_ptr<ICloudHttp> makePlatformHttp() {
    hb::net::HttpClientOptions o;
    o.userAgent = "HBStudio-Cloud/1.0";
    o.proxy = hb::net::ProxyMode::Automatic;
    return makeCloudHttpOverNet(hb::net::makeWinHttpClient(o));
}
std::shared_ptr<ISecretStore> makePlatformSecretStore(const std::string& path) {
    // Same entropy/description as the pre-src/net store: existing credential files keep decrypting.
    std::shared_ptr<hb::net::ISecretStore> dpapi = hb::net::makeDpapiSecretStore({"HBStudio.cloud.v1", "HBStudio cloud"});
    return makeFileSecretStore(path, std::move(dpapi));
}
std::shared_ptr<IFileSystem> makePlatformFileSystem() { return std::make_shared<WinFs>(); }

void setBackgroundThreadPriority() {
    // BACKGROUND_BEGIN lowers CPU, I/O and memory priority together: the live pipeline always wins.
    if (!::SetThreadPriority(::GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN)) ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_LOWEST);
}

}  // namespace hb::cloud
#endif
