#pragma once
#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "discovery/device.hpp"

namespace hb::discovery {

struct SourceReport {
    enum class Status { Ok, Failed, TimedOut, Exception };
    std::string name;
    Status status = Status::Ok;
    std::string message;
    std::size_t deviceCount = 0;  // devices contributed before deduplication
    std::chrono::milliseconds elapsed{0};
};

struct DiscoveryReport {
    std::vector<DiscoveredDevice> devices;  // deduplicated, order = source registration then discovery
    std::vector<SourceReport> sources;
};

// Merges devices that share an id or a non-empty IP: protocols/xaddrs are unioned, ptz is OR-ed,
// empty fields are filled from later duplicates. (Only direct matches; no transitive chains.)
std::vector<DiscoveredDevice> mergeDevices(const std::vector<DiscoveredDevice>& in);

// Runs all sources in parallel, each on its own thread with its own timeout. A source that throws,
// fails or exceeds its timeout is reported and does not affect the others. A timed-out source is
// signalled through its CancelToken and abandoned (its thread finishes on its own; late results are
// discarded), so run() never waits beyond the largest timeout.
class DiscoveryEngine {
public:
    void addSource(std::shared_ptr<IDiscoverySource> source, std::chrono::milliseconds timeout);
    DiscoveryReport run();

private:
    struct Entry {
        std::shared_ptr<IDiscoverySource> source;
        std::chrono::milliseconds timeout;
    };
    std::vector<Entry> sources_;
};

}  // namespace hb::discovery
