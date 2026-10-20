#pragma once
// Injectable clock for the PTZ module (tests use ManualClock, no sleeps).
#include "ptz/types.hpp"

namespace hb::ptz {

class IPtzClock {
public:
    virtual ~IPtzClock() = default;
    virtual TimePoint now() const = 0;
};

class SystemPtzClock : public IPtzClock {
public:
    TimePoint now() const override { return std::chrono::steady_clock::now(); }
};

class ManualClock : public IPtzClock {
public:
    TimePoint now() const override { return t_; }
    void advance(Millis d) { t_ += d; }

private:
    TimePoint t_{std::chrono::seconds(1000)};
};

}  // namespace hb::ptz
