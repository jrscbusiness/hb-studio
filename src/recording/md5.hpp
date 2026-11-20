#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace hb::rec::detail {

// RFC 1321 MD5 (own implementation; needed for the FLAC STREAMINFO signature).
class Md5 {
public:
    Md5() { reset(); }
    void reset();
    void update(const void* data, std::size_t len);
    std::array<std::uint8_t, 16> finish();

private:
    void block(const std::uint8_t* p);
    std::uint32_t s_[4];
    std::uint8_t buf_[64];
    std::size_t bufLen_ = 0;
    std::uint64_t total_ = 0;
};

}  // namespace hb::rec::detail
