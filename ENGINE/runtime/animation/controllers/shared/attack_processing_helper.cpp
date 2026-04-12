#include "animation/controllers/shared/attack_processing_helper.hpp"

#include "animation/animation_update.hpp"
#include "assets/asset/Asset.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

namespace animation_update::custom_controllers {

namespace {

constexpr float kZeroTolerance = 1e-4f;
constexpr char kHitAnimationId[] = "hit";
constexpr char kDieAnimationId[] = "die";
constexpr char kBreakAnimationId[] = "break";

} // namespace

bool AttackProcessingHelper::try_play_death_animation(Asset& self) {
    if (!self.info) {
        return false;
    }

    auto try_animation = [&self](const char* animation_id) {
        if (self.info->animations.find(animation_id) == self.info->animations.end()) {
            return false;
        }
        self.set_current_animation(animation_id);
        return true;
    };

    if (try_animation(kDieAnimationId)) {
        return true;
    }
    return try_animation(kBreakAnimationId);
}

bool AttackProcessingHelper::compute_knockback_delta(const Asset& self,
                                                      const animation_update::Attack& attack,
                                                      SDL_Point& out_delta,
                                                      float max_distance,
                                                      int max_damage) {
    if (!attack.payload.hitback_enabled) {
        return false;
    }

    float travel_distance = std::max(0.0f, attack.payload.hitback_distance);
    if (travel_distance <= kZeroTolerance) {
        if (max_distance <= 0.0f || max_damage <= 0) {
            return false;
        }
        const float clamped_damage = std::clamp(static_cast<float>(attack.payload.damage_amount),
                                                0.0f,
                                                static_cast<float>(max_damage));
        if (clamped_damage <= 0.0f) {
            return false;
        }
        const float damage_ratio = clamped_damage / static_cast<float>(max_damage);
        travel_distance = (damage_ratio * max_distance)/self.info->weight_kg; // Knockback is inversely proportional to weight.
    } else if (max_distance > 0.0f) {
        travel_distance = std::min(travel_distance, max_distance);
    }

    if (travel_distance <= kZeroTolerance) {
        return false;
    }

    const float source_x = static_cast<float>(self.world_x());
    const float source_z = static_cast<float>(self.world_z());
    const float attack_x = attack.hit_x;
    const float attack_z = attack.hit_z;
    const float dx = source_x - attack_x;
    const float dz = source_z - attack_z;
    const float magnitude = std::sqrt(dx * dx + dz * dz);
    if (magnitude <= kZeroTolerance) {
        return false;
    }

    const float scale = travel_distance / magnitude;
    const int bumped_x = static_cast<int>(std::round(dx * scale));
    const int bumped_z = static_cast<int>(std::round(dz * scale));
    if (bumped_x == 0 && bumped_z == 0) {
        return false;
    }

    out_delta.x = bumped_x;
    out_delta.y = bumped_z;
    return true;
}

void AttackProcessingHelper::apply_knockback(Asset& self, SDL_Point delta) {
    if (!self.anim_) {
        return;
    }
    self.anim_->cancel_all_movement();
    self.anim_->move(delta, animation_update::detail::kDefaultAnimation, true, true);
}

void AttackProcessingHelper::process_pending_attacks(Asset& self) {
    const auto pending_attacks = self.process_pending_attacks();
    bool took_damage = false;
    std::optional<SDL_Point> strongest_knockback{};
    for (const auto& attack : pending_attacks) {
        const int applied_damage = std::max(0, attack.payload.damage_amount);
        self.runtime_health -= applied_damage;
        took_damage = took_damage || applied_damage > 0;
        SDL_Point candidate_knockback{};
        if (!compute_knockback_delta(self, attack, candidate_knockback)) {
            continue;
        }
        if (!strongest_knockback.has_value()) {
            strongest_knockback = candidate_knockback;
            continue;
        }
        const int current_sq =
            strongest_knockback->x * strongest_knockback->x +
            strongest_knockback->y * strongest_knockback->y;
        const int candidate_sq =
            candidate_knockback.x * candidate_knockback.x +
            candidate_knockback.y * candidate_knockback.y;
        if (candidate_sq > current_sq) {
            strongest_knockback = candidate_knockback;
        }
    }

    if (self.runtime_health < 0) {
        if (!try_play_death_animation(self)) {
            self.Delete();
        }
        return;
    }

    if (strongest_knockback.has_value()) {
        apply_knockback(self, *strongest_knockback);
        return;
    }

    if (took_damage && self.info && self.info->animations.find(kHitAnimationId) != self.info->animations.end()) {
        self.set_current_animation(kHitAnimationId);
    }
}

} // namespace animation_update::custom_controllers
