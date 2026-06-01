#include "animation/controllers/shared/internal/enemy_positioning_system.hpp"

#include <algorithm>
#include <array>
#include <utility>

namespace animation_update::custom_controllers::internal {

std::vector<CombatPositionCandidate> EnemyPositioningSystem::ring_candidates(
    const axis::WorldPos& target_position,
    int desired_range_px) {
    const int range = std::max(0, desired_range_px);
    const std::array<axis::WorldPos, 4> offsets{{
        axis::WorldPos{-range, 0, 0},
        axis::WorldPos{ range, 0, 0},
        axis::WorldPos{0, 0, -range},
        axis::WorldPos{0, 0,  range},
    }};

    std::vector<CombatPositionCandidate> candidates;
    candidates.reserve(offsets.size());
    for (std::size_t i = 0; i < offsets.size(); ++i) {
        CombatPositionCandidate candidate{};
        candidate.position = axis::WorldPos{
            target_position.x + offsets[i].x,
            target_position.y,
            target_position.z + offsets[i].z};
        candidate.score = 1.0f - (static_cast<float>(i) * 0.05f);
        candidate.reachable = true;
        candidate.reason = "compat_ring_candidate";
        candidates.push_back(std::move(candidate));
    }
    return candidates;
}

const CombatPositionCandidate* EnemyPositioningSystem::best_candidate(
    const std::vector<CombatPositionCandidate>& candidates) {
    const CombatPositionCandidate* best = nullptr;
    for (const auto& candidate : candidates) {
        if (!candidate.reachable || candidate.reserved) {
            continue;
        }
        if (!best || candidate.score > best->score) {
            best = &candidate;
        }
    }
    return best;
}

} // namespace animation_update::custom_controllers::internal
