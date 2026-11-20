#pragma once
// Lists capture devices, system-audio loopback (WASAPI) and playback devices.
// Updated on demand with refresh(); thread-safe; never throws; a failing audio system
// yields an empty list plus a pt-BR message (lastError()), never a crash.
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "audio_io/raw_device.hpp"

namespace hb::audio_io {

class DeviceEnumerator {
public:
    using Lister = std::function<std::vector<DeviceInfo>(Backend, std::string& error)>;

    explicit DeviceEnumerator(Backend backend = Backend::Auto, Lister lister = {});

    // Re-reads the devices from the OS. Returns true when the listing succeeded (an empty list is
    // still a success). On failure the previous list is kept and lastError() explains.
    bool refresh();

    std::vector<DeviceInfo> devices() const;                    // all kinds
    std::vector<DeviceInfo> devices(DeviceKind kind) const;     // filtered
    std::optional<DeviceInfo> find(const std::string& id) const;
    std::string lastError() const;

    // {"devices":[{"id","name","kind","default","channels","sampleRate"}...],"error":""}
    std::string toJson() const;

private:
    Backend backend_;
    Lister lister_;
    mutable std::mutex mutex_;
    std::vector<DeviceInfo> devices_;
    std::string error_;
};

}  // namespace hb::audio_io
