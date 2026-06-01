#include "animation/controllers/shared/internal/enemy_positioning_system.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace animation_update::custom_controllers::internal {

namespace {

long long distance_sq(const axis::WorldPos& a, const axis::WorldPos& b) {
    const long long dx = static_cast<long long>(b.x) - a.x;
    const long long dz = static_cast<long long>(b.z) - a.z;
    return (dx * dx) + (dz * dz);
}

bool slot_overlaps(const CombatSlotReservation& reservation,
                   const axis::WorldPos& position,
                   std::chrono::steady_clock::time_point now) {
    if (now >= reservation.expires_at) {
        return false;
    }
    return distance_sq(reservation.position, position) < 32LL * 32LL;
}

} // namespace

std::vector<CombatPositionCandidate> EnemyPositioningSystem::ring_candidates(
    const axis::WorldPos& target_position,
    int desired_range_px) {
    const int range = std::max(0, desired_range_px);
    const std::array<axis::WorldPos, 8> offsets{{
        axis::WorldPos{-range, 0, 0},
        axis::WorldPos{ range, 0, 0},
        axis::WorldPos{0, 0, -range},
        axis::WorldPos{0, 0,  range},
        axis::WorldPos{-range, 0, -range},
        axis::WorldPos{ range, 0, -range},
        axis::WorldPos{-range, 0,  range},
        axis::WorldPos{ range, 0,  range},
    }};

    std::vector<CombatPositionCandidate> candidates;
    candidates.reserve(offsets.size());
    for (std::size_t i = 0; i < offsets.size(); ++i) {
        CombatPositionCandidate candidate{};
        candidate.position = axis::WorldPos{
            target_position.x + offsets[i].x,
            target_position.y,
            target_position.z + offsets[i].z};
        candidate.score = 1.0f - (static_cast<float>(i) * 0.03f);
        candidate.reachable = true;
        candidate.reason = "ring_candidate";
        candidates.push_back(std::move(candidate));
    }
    return candidates;
}

std::vector<CombatPositionCandidate> EnemyPositioningSystem::scored_candidates(
    const axis::WorldPos& self_position,
    const axis::WorldPos& target_position,
    int desired_range_px,
    const CombatSlotTable& slots,
    const std::string& target_id,
    std::chrono::steady_clock::time_point now) {
    std::vector<CombatPositionCandidate> candidates = ring_candidates(target_position, desired_range_px);
    for (CombatPositionCandidate& candidate : candidates) {
        const double self_dist = std::sqrt(static_cast<double>(std::max<long long>(1, distance_sq(self_position, candidate.position))));
        const double target_dist = std::sqrt(static_cast<double>(std::max<long long>(1, distance_sq(target_position, candidate.position))));
        const double desired_error = std::abs(target_dist - static_cast<double>(std::max(1, desired_range_px)));
        candidate.score += static_cast<float>(1.0 / (1.0 + desired_error));
        candidate.score += static_cast<float>(0.25 / (1.0 + (self_dist / 128.0)));
        for (const auto& [_, reservation] : slots.reservations_by_enemy) {
            if (reservation.target_id != target_id) {
                continue;
            }
            if (slot_overlaps(reservation, candidate.position, now)) {
                candidate.reserved = true;
                candidate.score -= 10.0f;
                candidate.reason = "reserved_slot";
                break;
            }
        }
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

std::optional<CombatSlotReservation> EnemyPositioningSystem::reserve_best_slot(
    CombatSlotTable& slots,
    const std::string& enemy_id,
    const std::string& target_id,
    const std::vector<CombatPositionCandidate>& candidates,
    std::chrono::steady_clock::time_point now,
    std::chrono::milliseconds duration) {
    prune_expired(slots, now);
    if (active_reservation_count(slots, target_id, now) >= std::max(1, slots.active_attacker_limit)) {
        return std::nullopt;
    }
    const CombatPositionCandidate* best = best_candidate(candidates);
    if (!best) {
        return std::nullopt;
    }
    CombatSlotReservation reservation{};
    reservation.enemy_id = enemy_id;
    reservation.target_id = target_id;
    reservation.position = best->position;
    reservation.expires_at = now + std::max(std::chrono::milliseconds(1), duration);
    slots.reservations_by_enemy[enemy_id] = reservation;
    return reservation;
}

int EnemyPositioningSystem::active_reservation_count(const CombatSlotTable& slots,
                                                     const std::string& target_id,
                                                     std::chrono::steady_clock::time_point now) {
    int count = 0;
    for (const auto& [_, reservation] : slots.reservations_by_enemy) {
        if (reservation.target_id == target_id && now < reservation.expires_at) {
            ++count;
        }
    }
    return count;
}

void EnemyPositioningSystem::prune_expired(CombatSlotTable& slots,
                                           std::chrono::steady_clock::time_point now) {
    for (auto it = slots.reservations_by_enemy.begin(); it != slots.reservations_by_enemy.end();) {
        if (now >= it->second.expires_at) {
            it = slots.reservations_by_enemy.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace animation_update::custom_controllers::internal
