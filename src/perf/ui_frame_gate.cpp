#include "perf/ui_frame_gate.hpp"

#include <cstring>

namespace hb::perf {

std::uint64_t hashFrame(const video::Frame& f) {
    constexpr std::uint64_t kMul = 0x9E3779B97F4A7C15ull;
    std::uint64_t h0 = 0x243F6A8885A308D3ull ^ static_cast<std::uint64_t>(f.width),
                  h1 = 0x13198A2E03707344ull ^ static_cast<std::uint64_t>(f.height), h2 = 0xA4093822299F31D0ull,
                  h3 = 0x082EFA98EC4E6C89ull;
    if (f.width <= 0 || f.height <= 0 || f.stride < f.width * 4 ||
        f.data.size() < static_cast<std::size_t>(f.stride) * static_cast<std::size_t>(f.height))
        return h0 ^ h1;
    const std::size_t rowBytes = static_cast<std::size_t>(f.width) * 4;
    for (int y = 0; y < f.height; ++y) {
        const std::uint8_t* p = f.data.data() + static_cast<std::size_t>(y) * f.stride;
        std::size_t i = 0;
        for (; i + 32 <= rowBytes; i += 32) {   // 4 independent lanes: no serial dependency chain
            std::uint64_t w[4];
            std::memcpy(w, p + i, 32);
            h0 = (h0 ^ w[0]) * kMul; h0 ^= h0 >> 29;
            h1 = (h1 ^ w[1]) * kMul; h1 ^= h1 >> 29;
            h2 = (h2 ^ w[2]) * kMul; h2 ^= h2 >> 29;
            h3 = (h3 ^ w[3]) * kMul; h3 ^= h3 >> 29;
        }
        for (; i < rowBytes; i += 4) {
            std::uint32_t w;
            std::memcpy(&w, p + i, 4);
            h0 = (h0 ^ w) * kMul; h0 ^= h0 >> 29;
        }
    }
    std::uint64_t h = h0;
    h = (h ^ h1) * kMul; h ^= h >> 31;
    h = (h ^ h2) * kMul; h ^= h >> 31;
    h = (h ^ h3) * kMul; h ^= h >> 31;
    return h;
}

bool UiFrameGate::shouldSend(const std::string& id, const video::Frame& f, double nowSec) {
    const std::uint64_t h = hashFrame(f);
    auto it = state_.find(id);
    if (it != state_.end() && it->second.hash == h && nowSec - it->second.lastSent < keepAliveSec_) {
        ++suppressed_;
        return false;
    }
    State& s = state_[id];
    s.hash = h;
    s.lastSent = nowSec;
    ++sent_;
    return true;
}

bool UiFramePolicy::isHotTab(const std::string& tab) { return tab == "prod" || tab == "gfx" || tab == "cam"; }

void UiFramePolicy::setActiveTab(const std::string& tab) {
    if (tab.empty() || tab.size() > 16) return;
    std::lock_guard lock(mutex_);
    tab_ = tab;
    hot_.store(isHotTab(tab));
}

std::string UiFramePolicy::activeTab() const {
    std::lock_guard lock(mutex_);
    return tab_;
}

}  // namespace hb::perf
