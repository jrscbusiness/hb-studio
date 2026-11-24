// DPAPI-backed ISecretStore. Windows only. NOT VALIDATED: written without a Windows compiler.
#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincrypt.h>

#include "ai/secret_store.hpp"

#pragma comment(lib, "crypt32.lib")

namespace hb::ai {
namespace {

// Extra entropy: binds blobs to this app so another program running as the same user cannot trivially
// unprotect them with a bare CryptUnprotectData call (not a defence against a determined local attacker).
const char kEntropy[] = "HBStudio.ai.v1";

class DpapiSecretStore : public ISecretStore {
public:
    bool protect(const std::string& plain, std::string& blob) override {
        DATA_BLOB in{static_cast<DWORD>(plain.size()), reinterpret_cast<BYTE*>(const_cast<char*>(plain.data()))};
        DATA_BLOB ent{static_cast<DWORD>(sizeof(kEntropy) - 1), reinterpret_cast<BYTE*>(const_cast<char*>(kEntropy))};
        DATA_BLOB out{0, nullptr};
        if (!::CryptProtectData(&in, L"HB Studio AI key", &ent, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return false;
        blob.assign(reinterpret_cast<const char*>(out.pbData), out.cbData);
        ::LocalFree(out.pbData);
        return true;
    }
    bool unprotect(const std::string& blob, std::string& plain) override {
        DATA_BLOB in{static_cast<DWORD>(blob.size()), reinterpret_cast<BYTE*>(const_cast<char*>(blob.data()))};
        DATA_BLOB ent{static_cast<DWORD>(sizeof(kEntropy) - 1), reinterpret_cast<BYTE*>(const_cast<char*>(kEntropy))};
        DATA_BLOB out{0, nullptr};
        if (!::CryptUnprotectData(&in, nullptr, &ent, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return false;
        plain.assign(reinterpret_cast<const char*>(out.pbData), out.cbData);
        ::SecureZeroMemory(out.pbData, out.cbData);
        ::LocalFree(out.pbData);
        return true;
    }
};

}  // namespace

std::unique_ptr<ISecretStore> makeDefaultSecretStore() { return std::make_unique<DpapiSecretStore>(); }

}  // namespace hb::ai

#endif  // _WIN32
