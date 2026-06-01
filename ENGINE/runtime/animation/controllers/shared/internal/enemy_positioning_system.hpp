#pragma once

#include <chrono>
#include <string>
#include <vector>

#include "core/axis_convention.hpp"

namespace animation_update::custom_controllers::internal {

struct CombatPositionCandidate {
    axis::WorldPos position{0, 0, 0};
    float score = 0.0f;
    bool reachable = false;
    bool reserved = false;
    std::string reason;
};

struct CombatSlotReservation {
    std::string enemy_id;
    std::string target_id;
    axis::WorldPos position{0, 0, 0};
    std::chrono::steady_clock::time_point expires_at{};
};

class EnemyPositioningSystem {
public:
    static std::vector<CombatPositionCandidate> ring_candidates(const axis::WorldPos& target_position,
                                                                int desired_range_px);
    static const CombatPositionCandidate* best_candidate(const std::vector<CombatPositionCandidate>& candidates);
};

} // namespace animation_update::custom_controllers::internal
