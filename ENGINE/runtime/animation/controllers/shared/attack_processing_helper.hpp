#pragma once

#include <SDL3/SDL.h>
#include <string_view>

namespace animation_update {
struct Attack;
}

class Asset;

namespace animation_update::custom_controllers {

struct AttackProcessingConfig {
    float max_knockback_distance = 50.0f;
    int max_damage_for_knockback = 100;
    float knockback_scale = 1.0f;
    std::string_view hit_animation_id = "hit";
    std::string_view death_animation_id = "die";
    std::string_view hit_fallback_animation_id = "default";
    std::string_view death_fallback_tag = "break";
};

class AttackProcessingHelper {
public:
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

} // namespace animation_update::custom_controllers
