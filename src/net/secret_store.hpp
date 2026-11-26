#pragma once
// Encryption-at-rest for secrets, behind an interface. Windows: DPAPI (CryptProtectData, bound to the
// current Windows user, optional per-purpose entropy; secret_store_win.cpp). Tests: FakeSecretStore.
// This is the low-level primitive (opaque blobs); modules decide where blobs are persisted.
#include <memory>
#include <string>

namespace hb::net {

class ISecretStore {
public:
    virtual ~ISecretStore() = default;
    // `blob` is opaque binary; only the same implementation + entropy (same user) can unprotect it.
    virtual bool protect(const std::string& plain, std::string& blob) = 0;
    virtual bool unprotect(const std::string& blob, std::string& plain) = 0;
};

// NOT SECURE - reversible obfuscation, for unit tests and non-Windows development builds.
class FakeSecretStore : public ISecretStore {
public:
    bool protect(const std::string& plain, std::string& blob) override;
    bool unprotect(const std::string& blob, std::string& plain) override;
    bool failProtect = false;  // test hooks
    bool failUnprotect = false;
};

struct SecretStoreOptions {
    // Binds blobs to one purpose of this app (another program running as the same user cannot trivially
    // unprotect them with a bare CryptUnprotectData call; not a defence against a determined local attacker).
    // Empty = no entropy.
    std::string entropy;
    std::string description;  // shown by DPAPI metadata only; never secret
};

// DPAPI on Windows; nullptr elsewhere (callers keep secrets in memory only, never on disk).
std::unique_ptr<ISecretStore> makeDpapiSecretStore(const SecretStoreOptions& options = {});

}  // namespace hb::net
