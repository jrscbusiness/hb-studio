#include "ptz/provider_selection.hpp"

namespace hb::ptz {

int selectProviderIndex(const std::vector<const PTZCapabilities*>& c, const PTZCapabilities& required) {
    int best = -1, bestScore = -1;
    for (std::size_t i = 0; i < c.size(); ++i) {
        if (!c[i] || !(c[i]->panTilt || c[i]->zoom)) continue;
        if (c[i]->covers(required)) return static_cast<int>(i);
        const int score = c[i]->coverCount(required);
        if (score > bestScore) {
            bestScore = score;
            best = static_cast<int>(i);
        }
    }
    return best;
}

std::unique_ptr<PTZProvider> selectProvider(std::vector<std::unique_ptr<PTZProvider>>& candidates,
                                            const PTZCapabilities& required) {
    std::vector<const PTZCapabilities*> caps;
    for (auto& p : candidates) caps.push_back(p ? &p->capabilities() : nullptr);
    const int i = selectProviderIndex(caps, required);
    if (i < 0) return nullptr;
    return std::move(candidates[static_cast<std::size_t>(i)]);
}

}  // namespace hb::ptz
