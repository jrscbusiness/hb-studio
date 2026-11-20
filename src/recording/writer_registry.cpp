#include "recording/media_writer.hpp"

namespace hb::rec {

WriterRegistry& WriterRegistry::instance() {
    static WriterRegistry r;
    return r;
}

bool WriterRegistry::add(const RecordingProfile& profile, WriterFactory factory) {
    if (profile.id.empty() || !factory) return false;
    std::lock_guard lock(mutex_);
    for (const auto& it : items_)
        if (it.first.id == profile.id) return false;
    items_.emplace_back(profile, std::move(factory));
    return true;
}

std::vector<RecordingProfile> WriterRegistry::profiles() const {
    std::lock_guard lock(mutex_);
    std::vector<RecordingProfile> out;
    for (const auto& it : items_) out.push_back(it.first);
    return out;
}

bool WriterRegistry::find(const std::string& id, RecordingProfile& profile, WriterFactory& factory) const {
    std::lock_guard lock(mutex_);
    for (const auto& it : items_)
        if (it.first.id == id) {
            profile = it.first;
            factory = it.second;
            return true;
        }
    return false;
}

}  // namespace hb::rec
