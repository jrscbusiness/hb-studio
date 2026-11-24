// POSIX CSPRNG: getrandom() on Linux, /dev/urandom elsewhere.
#ifndef _WIN32
#include <cstdio>

#include "remote/random.hpp"

#if defined(__linux__)
#include <sys/random.h>

#include <cerrno>
#endif

namespace hb::remote {
namespace {
class PosixRandom final : public IRandom {
public:
    bool bytes(std::uint8_t* buf, std::size_t len) override {
        std::size_t off = 0;
#if defined(__linux__)
        while (off < len) {
            const auto n = ::getrandom(buf + off, len - off, 0);
            if (n > 0) {
                off += static_cast<std::size_t>(n);
                continue;
            }
            if (n < 0 && errno == EINTR) continue;
            break;  // ENOSYS etc.: fall back to /dev/urandom below
        }
        if (off == len) return true;
#endif
        std::FILE* f = std::fopen("/dev/urandom", "rb");
        if (!f) return false;
        while (off < len) {
            const std::size_t n = std::fread(buf + off, 1, len - off, f);
            if (n == 0) {
                std::fclose(f);
                return false;
            }
            off += n;
        }
        std::fclose(f);
        return true;
    }
};
}  // namespace

IRandom& systemRandom() {
    static PosixRandom r;
    return r;
}
}  // namespace hb::remote
#endif  // !_WIN32
