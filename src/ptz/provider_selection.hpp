#pragma once
#include <memory>
#include <vector>

#include "ptz/controller.hpp"

namespace hb::ptz {

// Picks the best provider among the candidates (already ordered by preference,
// e.g. ONVIF, VISCA IP, VISCA serial, Pelco-D, Pelco-P):
//  1. the first candidate whose capabilities cover everything in `required`;
//  2. otherwise the candidate covering the most required features (ties: first);
//  3. a candidate with no pan/tilt/zoom at all is never chosen.
// Returns the index, or -1 when PTZ must be disabled.
int selectProviderIndex(const std::vector<const PTZCapabilities*>& candidates, const PTZCapabilities& required);

// Convenience: moves the winner out of `candidates` (nullptr if none).
std::unique_ptr<PTZProvider> selectProvider(std::vector<std::unique_ptr<PTZProvider>>& candidates,
                                            const PTZCapabilities& required);

}  // namespace hb::ptz
