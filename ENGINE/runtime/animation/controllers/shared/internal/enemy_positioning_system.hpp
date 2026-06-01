#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <unordered_map>
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

struct CombatSlotTable {
    std::unordered_map<std::string, CombatSlotReservation> reservations_by_enemy;
    int active_attacker_limit = 2;
};

class EnemyPositioningSystem {
public:
    static std::vector<CombatPositionCandidate> ring_candidates(const axis::WorldPos& target_position,
                                                                int desired_range_px);
    static std::vector<CombatPositionCandidate> scored_candidates(const axis::WorldPos& self_position,
                                                                  const axis::WorldPos& target_position,
                                                                  int desired_range_px,
                                                                  const CombatSlotTable& slots,
                                                                  const std::string& target_id,
                                                                  std::chrono::steady_clock::time_point now);
    static const CombatPositionCandidate* best_candidate(const std::vector<CombatPositionCandidate>& candidates);
    static std::optional<CombatSlotReservation> reserve_best_slot(CombatSlotTable& slots,
                                                                  const std::string& enemy_id,
                                                                  const std::string& target_id,
                                                                  const std::vector<CombatPositionCandidate>& candidates,
                                                                  std::chrono::steady_clock::time_point now,
                                                                  std::chrono::milliseconds duration);
    static int active_reservation_count(const CombatSlotTable& slots,
                                        const std::string& target_id,
                                        std::chrono::steady_clock::time_point now);
    static void prune_expired(CombatSlotTable& slots, std::chrono::steady_clock::time_point now);
};

} // namespace animation_update::custom_controllers::internal
