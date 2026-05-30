#pragma once

#include <SDL3/SDL.h>
#include <vector>

#include "animation/controllers/shared/controller_types.hpp"

namespace animation_update {
struct Attack;
}

class Asset;

namespace animation_update::custom_controllers::internal {

class AttackProcessingHelper {
public:
    static AttackProcessingSummary process_attacks(
        Asset& self,
        const std::vector<animation_update::Attack>& attacks,
        const AttackProcessingConfig& config = AttackProcessingConfig{});
    static void process_pending_attacks(
        Asset& self,
        const AttackProcessingConfig& config = AttackProcessingConfig{});
    static bool try_play_death_animation(
        Asset& self,
        const AttackProcessingConfig& config = AttackProcessingConfig{});
    static bool compute_knockback_delta(const Asset& self,
                                        const animation_update::Attack& attack,
                                        SDL_Point& out_delta,
                                        float max_distance = 50.0f,
                                        int max_damage = 100);
    static void apply_knockback(Asset& self, SDL_Point delta);
};

} // namespace animation_update::custom_controllers::internal
