#pragma once

#include <SDL3/SDL.h>

namespace animation_update {
struct Attack;
}

class Asset;

namespace animation_update::custom_controllers {

class AttackReactionHelper {
public:
    static bool try_play_death_animation(Asset& self);
    static bool compute_knockback_delta(const Asset& self,
                                       const animation_update::Attack& attack,
                                       SDL_Point& out_delta,
                                       float max_distance = 50.0f,
                                       int max_damage = 100);
    static void apply_knockback(Asset& self, SDL_Point delta);
};

} // namespace animation_update::custom_controllers
