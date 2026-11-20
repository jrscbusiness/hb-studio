#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace hb::core {

// FIPS 180-4 SHA-256 (own implementation: no dependency, tested with the NIST vectors).
class Sha256 {
public:
    Sha256() { reset(); }
    void reset();
    void update(const void* data, std::size_t len);
    void update(std::string_view s) { update(s.data(), s.size()); }
    // Finishes the hash (the object must be reset() before reuse). Returns 64 lowercase hex chars.
    std::string hexDigest();

    static std::string hex(std::string_view data) {
        Sha256 h;
        h.update(data);
        return h.hexDigest();
    }

private:
    void block(const std::uint8_t* p);
    std::uint32_t h_[8];
    std::uint8_t buf_[64];
    std::size_t bufLen_ = 0;
    std::uint64_t total_ = 0;
};

}  // namespace hb::core
