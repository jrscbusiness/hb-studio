#include "audio/effect.hpp"

namespace hb::audio {

EffectRegistry& EffectRegistry::instance() {
    static EffectRegistry r;
    return r;
}

bool EffectRegistry::add(const Info& info, Factory factory) {
    if (info.typeId.empty() || !factory) return false;
    std::lock_guard lock(mutex_);
    return items_.emplace(info.typeId, std::make_pair(info, std::move(factory))).second;
}

void EffectRegistry::removeBySource(const std::string& source) {
    std::lock_guard lock(mutex_);
    for (auto it = items_.begin(); it != items_.end();) {
        if (it->second.first.source == source) it = items_.erase(it);
        else ++it;
    }
}

std::unique_ptr<Effect> EffectRegistry::create(const std::string& typeId) const {
    Factory f;
    {
        std::lock_guard lock(mutex_);
        auto it = items_.find(typeId);
        if (it == items_.end()) return nullptr;
        f = it->second.second;
    }
    try {
        return f();
    } catch (...) {
        return nullptr;
    }
}

std::vector<EffectRegistry::Info> EffectRegistry::list() const {
    std::lock_guard lock(mutex_);
    std::vector<Info> out;
    for (const auto& [id, p] : items_) out.push_back(p.first);
    return out;
}

bool EffectRegistry::contains(const std::string& typeId) const {
    std::lock_guard lock(mutex_);
    return items_.count(typeId) != 0;
}

}  // namespace hb::audio
