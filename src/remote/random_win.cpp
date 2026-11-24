// Windows CSPRNG (BCryptGenRandom). NOT VALIDATED on Windows (written without a Windows compiler).
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>

#include "remote/random.hpp"

#pragma comment(lib, "bcrypt.lib")

namespace hb::remote {
namespace {
class WinRandom final : public IRandom {
public:
    bool bytes(std::uint8_t* buf, std::size_t len) override {
        while (len > 0) {
            const ULONG chunk = len > 0x10000 ? 0x10000 : static_cast<ULONG>(len);
            // Return value 0 == STATUS_SUCCESS (avoids needing ntstatus.h / NT_SUCCESS here).
            if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(buf), chunk, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
                return false;
            buf += chunk;
            len -= chunk;
        }
        return true;
    }
};
}  // namespace

IRandom& systemRandom() {
    static WinRandom r;
    return r;
}
}  // namespace hb::remote
#endif  // _WIN32
