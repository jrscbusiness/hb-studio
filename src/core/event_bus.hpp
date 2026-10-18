#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace hb::core {

using Fields = std::map<std::string, std::string>;

struct Event {
    std::string topic;  // e.g. "camera.connected"
    Fields fields;
};

using Handler = std::function<void(const Event&)>;
using SubscriptionId = std::uint64_t;

// Synchronous, thread-safe publish/subscribe. A throwing handler never
// affects other handlers or the publisher (isolation requirement).
//
// Patterns: exact topic ("switcher.programChanged"), prefix ("camera.*")
// or everything ("*").
class EventBus {
public:
    SubscriptionId subscribe(std::string pattern, Handler handler);
    void unsubscribe(SubscriptionId id);

    // Returns the number of handlers that ran without throwing.
    std::size_t publish(const Event& event);

    // Number of handler exceptions swallowed so far.
    std::uint64_t handlerErrors() const { return errors_.load(); }

    static bool matches(std::string_view pattern, std::string_view topic);

private:
    struct Sub {
        SubscriptionId id;
        std::string pattern;
        Handler handler;
    };
    mutable std::mutex mutex_;
    std::vector<Sub> subs_;
    SubscriptionId nextId_ = 1;
    std::atomic<std::uint64_t> errors_{0};
};

}  // namespace hb::core
