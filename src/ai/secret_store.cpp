#include "ai/secret_store.hpp"

#include <algorithm>

namespace hb::ai {

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

std::unique_ptr<ISecretStore> makeDefaultSecretStore() {
    // Same entropy/description as the pre-src/net implementation: existing blobs keep decrypting.
    return hb::net::makeDpapiSecretStore({"HBStudio.ai.v1", "HB Studio AI key"});
}

}  // namespace hb::ai
