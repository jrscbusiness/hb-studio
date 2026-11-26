#include "net/secret_store.hpp"

namespace hb::net {

bool FakeSecretStore::protect(const std::string& plain, std::string& blob) {
    if (failProtect) return false;
    blob = "FAKE1:" + std::string(plain.rbegin(), plain.rend());
    return true;
}

bool FakeSecretStore::unprotect(const std::string& blob, std::string& plain) {
    if (failUnprotect || blob.rfind("FAKE1:", 0) != 0) return false;
    const std::string body = blob.substr(6);
    plain.assign(body.rbegin(), body.rend());
    return true;
}

#ifndef _WIN32
std::unique_ptr<ISecretStore> makeDpapiSecretStore(const SecretStoreOptions&) { return nullptr; }
#endif

}  // namespace hb::net
