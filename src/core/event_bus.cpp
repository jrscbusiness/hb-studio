#include "core/event_bus.hpp"

#include <algorithm>

namespace hb::core {

SubscriptionId EventBus::subscribe(std::string pattern, Handler handler) {
    std::lock_guard lock(mutex_);
    const auto id = nextId_++;
    subs_.push_back({id, std::move(pattern), std::move(handler)});
    return id;
}

void EventBus::unsubscribe(SubscriptionId id) {
    std::lock_guard lock(mutex_);
    subs_.erase(std::remove_if(subs_.begin(), subs_.end(),
                               [id](const Sub& s) { return s.id == id; }),
                subs_.end());
}

bool EventBus::matches(std::string_view pattern, std::string_view topic) {
    if (pattern == "*") return true;
    if (pattern.size() >= 2 && pattern.substr(pattern.size() - 2) == ".*") {
        return topic.substr(0, pattern.size() - 1) == pattern.substr(0, pattern.size() - 1);
    }
    return pattern == topic;
}

std::size_t EventBus::publish(const Event& event) {
    // Copy matching handlers so they run outside the lock; handlers may
    // therefore subscribe/unsubscribe/publish without deadlocking.
    std::vector<Handler> targets;
    {
        std::lock_guard lock(mutex_);
        for (const auto& s : subs_)
            if (matches(s.pattern, event.topic)) targets.push_back(s.handler);
    }
    std::size_t ok = 0;
    for (const auto& h : targets) {
        try {
            h(event);
            ++ok;
        } catch (...) {
            errors_.fetch_add(1);
        }
    }
    return ok;
}

}  // namespace hb::core
