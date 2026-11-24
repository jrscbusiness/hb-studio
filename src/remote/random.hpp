#pragma once
// Source of cryptographically secure random bytes, behind an interface so tests can inject a
// deterministic one. The system implementation is the OS CSPRNG (BCryptGenRandom on Windows,
// getrandom()/dev/urandom on POSIX) - never <random>/rand().
#include <cstddef>
#include <cstdint>

namespace hb::remote {

class IRandom {
public:
    virtual ~IRandom() = default;
    // Fills buf with len random bytes. Returns false if the OS source failed (callers must then
    // refuse to mint any secret - never fall back to a weak generator).
    virtual bool bytes(std::uint8_t* buf, std::size_t len) = 0;
};

// Shared process-wide OS CSPRNG (thread-safe).
IRandom& systemRandom();

}  // namespace hb::remote
