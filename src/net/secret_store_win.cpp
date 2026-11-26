// DPAPI-backed ISecretStore. Windows only. NOT VALIDATED on real hardware (verified by the Windows CI build).
#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincrypt.h>

#include "net/secret_store.hpp"

#pragma comment(lib, "crypt32.lib")

namespace hb::net {
namespace {

std::wstring widenAscii(const std::string& s) {
    if (s.empty()) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
    return w;
}

class DpapiSecretStore : public ISecretStore {
public:
    explicit DpapiSecretStore(const SecretStoreOptions& o) : entropy_(o.entropy), description_(widenAscii(o.description)) {}

    bool protect(const std::string& plain, std::string& blob) override {
        DATA_BLOB in{static_cast<DWORD>(plain.size()), reinterpret_cast<BYTE*>(const_cast<char*>(plain.data()))};
        DATA_BLOB ent = entropyBlob();
        DATA_BLOB out{0, nullptr};
        if (!::CryptProtectData(&in, description_.empty() ? nullptr : description_.c_str(), entropy_.empty() ? nullptr : &ent, nullptr,
                                nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out))
            return false;
        blob.assign(reinterpret_cast<const char*>(out.pbData), out.cbData);
        ::LocalFree(out.pbData);
        return true;
    }

    bool unprotect(const std::string& blob, std::string& plain) override {
        DATA_BLOB in{static_cast<DWORD>(blob.size()), reinterpret_cast<BYTE*>(const_cast<char*>(blob.data()))};
        DATA_BLOB ent = entropyBlob();
        DATA_BLOB out{0, nullptr};
        if (!::CryptUnprotectData(&in, nullptr, entropy_.empty() ? nullptr : &ent, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out))
            return false;
        plain.assign(reinterpret_cast<const char*>(out.pbData), out.cbData);
        ::SecureZeroMemory(out.pbData, out.cbData);
        ::LocalFree(out.pbData);
        return true;
    }

private:
    DATA_BLOB entropyBlob() const {
        return DATA_BLOB{static_cast<DWORD>(entropy_.size()), reinterpret_cast<BYTE*>(const_cast<char*>(entropy_.data()))};
    }
    std::string entropy_;
    std::wstring description_;
};

}  // namespace

std::unique_ptr<ISecretStore> makeDpapiSecretStore(const SecretStoreOptions& options) {
    return std::make_unique<DpapiSecretStore>(options);
}

}  // namespace hb::net

#endif  // _WIN32
