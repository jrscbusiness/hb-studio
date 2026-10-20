#pragma once
// Byte transport behind an interface: serial / UDP / TCP / fake.
// No method throws.
#include <cstdint>
#include <string>
#include <vector>

#include "ptz/types.hpp"

namespace hb::ptz {

using Bytes = std::vector<std::uint8_t>;

struct TransportResult {
    bool ok = true;
    bool timedOut = false;
    std::string error;
    static TransportResult success() { return {}; }
    static TransportResult fail(std::string e) { return {false, false, std::move(e)}; }
    static TransportResult timeout() { return {false, true, "timeout"}; }
};

class ITransport {
public:
    virtual ~ITransport() = default;
    virtual TransportResult open() = 0;
    virtual void close() = 0;
    virtual bool isOpen() const = 0;
    virtual TransportResult send(const Bytes& data) = 0;
    // Waits up to `timeout` for data; returns one datagram/chunk in `out`.
    virtual TransportResult receive(Bytes& out, Millis timeout) = 0;
};

}  // namespace hb::ptz
