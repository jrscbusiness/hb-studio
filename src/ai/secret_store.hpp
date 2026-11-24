#pragma once
// Encryption-at-rest for API keys, behind an interface. Windows: DPAPI (CryptProtectData, bound to
// the current Windows user; secret_store_win.cpp, NOT VALIDATED here). Tests: a reversible fake.
#include <memory>
#include <string>

namespace hb::ai {

class ISecretStore {
public:
    virtual ~ISecretStore() = default;
    // `blob` is opaque binary; only the same store implementation (same user) can unprotect it.
    virtual bool protect(const std::string& plain, std::string& blob) = 0;
    virtual bool unprotect(const std::string& blob, std::string& plain) = 0;
};

// NOT SECURE - obfuscation only, for unit tests and non-Windows development builds.
class FakeSecretStore : public ISecretStore {
public:
    bool protect(const std::string& plain, std::string& blob) override;
    bool unprotect(const std::string& blob, std::string& plain) override;
    bool failProtect = false;   // test hooks
    bool failUnprotect = false;
};

// DPAPI on Windows; nullptr elsewhere (keys are then kept in memory only, never written to disk).
std::unique_ptr<ISecretStore> makeDefaultSecretStore();

std::string toHex(const std::string& bytes);
bool fromHex(const std::string& hex, std::string& bytes);

}  // namespace hb::ai
