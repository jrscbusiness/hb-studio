#include "audio_io/device_enumerator.hpp"

#include <algorithm>
#include <exception>

#include "core/json.hpp"

namespace hb::audio_io {

DeviceEnumerator::DeviceEnumerator(Backend backend, Lister lister)
    : backend_(backend), lister_(lister ? std::move(lister) : Lister(listDevicesNative)) {}

bool DeviceEnumerator::refresh() {
    std::vector<DeviceInfo> list;
    std::string err;
    try {
        list = lister_(backend_, err);
    } catch (const std::exception& e) {
        err = std::string("Falha ao listar dispositivos de áudio: ") + e.what();
    } catch (...) {
        err = "Falha ao listar dispositivos de áudio.";
    }
    std::lock_guard lock(mutex_);
    error_ = err;
    if (err.empty()) {
        devices_ = std::move(list);
        return true;
    }
    if (!list.empty()) devices_ = std::move(list);  // partial listing is still better than nothing
    return false;
}

std::vector<DeviceInfo> DeviceEnumerator::devices() const {
    std::lock_guard lock(mutex_);
    return devices_;
}

std::vector<DeviceInfo> DeviceEnumerator::devices(DeviceKind kind) const {
    std::lock_guard lock(mutex_);
    std::vector<DeviceInfo> out;
    for (const auto& d : devices_)
        if (d.kind == kind) out.push_back(d);
    return out;
}

std::optional<DeviceInfo> DeviceEnumerator::find(const std::string& id) const {
    std::lock_guard lock(mutex_);
    auto it = std::find_if(devices_.begin(), devices_.end(), [&](const DeviceInfo& d) { return d.id == id; });
    if (it == devices_.end()) return std::nullopt;
    return *it;
}

std::string DeviceEnumerator::lastError() const {
    std::lock_guard lock(mutex_);
    return error_;
}

std::string DeviceEnumerator::toJson() const {
    std::lock_guard lock(mutex_);
    std::string o = "{\"devices\":[";
    bool first = true;
    for (const auto& d : devices_) {
        if (!first) o += ',';
        first = false;
        o += "{\"id\":\"" + core::json::escape(d.id) + "\",\"name\":\"" + core::json::escape(d.name) +
             "\",\"kind\":\"" + deviceKindName(d.kind) + "\",\"default\":" + (d.isDefault ? "true" : "false") +
             ",\"channels\":" + std::to_string(d.channels) + ",\"sampleRate\":" + std::to_string(d.sampleRate) + "}";
    }
    o += "],\"error\":\"" + core::json::escape(error_) + "\"}";
    return o;
}

}  // namespace hb::audio_io
