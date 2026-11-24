#include "ai/secret_store.hpp"

#include <algorithm>

namespace hb::ai {

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

std::string toHex(const std::string& bytes) {
    static const char* d = "0123456789abcdef";
    std::string o;
    o.reserve(bytes.size() * 2);
    for (unsigned char c : bytes) {
        o += d[c >> 4];
        o += d[c & 15];
    }
    return o;
}

bool fromHex(const std::string& hex, std::string& bytes) {
    if (hex.size() % 2 != 0 || hex.size() > 64 * 1024) return false;
    auto v = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string o;
    o.reserve(hex.size() / 2);
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        const int a = v(hex[i]), b = v(hex[i + 1]);
        if (a < 0 || b < 0) return false;
        o += static_cast<char>((a << 4) | b);
    }
    bytes = std::move(o);
    return true;
}

#ifndef _WIN32
std::unique_ptr<ISecretStore> makeDefaultSecretStore() { return nullptr; }
#endif

}  // namespace hb::ai
