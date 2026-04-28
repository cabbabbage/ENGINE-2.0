#include "animation/controllers/shared/attack_processing_helper.hpp"

#include "animation/animation_update.hpp"
#include "assets/asset/Asset.hpp"

#include <algorithm>
#include <cmath>
#include <optional>

namespace animation_update::custom_controllers {

namespace {

constexpr float kZeroTolerance = 1e-4f;

bool try_set_animation(Asset& self, std::string_view animation_id) {
    if (!self.anim_ || !self.info || animation_id.empty()) {
        return false;
    }
    const std::string animation_key{animation_id};
    if (self.info->animations.find(animation_key) == self.info->animations.end()) {
        return false;
    }
    self.anim_->set_animation(animation_key);
    return self.current_animation == animation_key;
}

} // namespace

bool AttackProcessingHelper::try_play_death_animation(
    Asset& self,
    const AttackProcessingConfig& config) {
    if (!self.info || !self.anim_) {
        return false;
    }

    if (try_set_animation(self, config.death_animation_id)) {
        return true;
    }
    if (!config.death_fallback_tag.empty()) {
        return self.anim_->set_animation_by_tags({std::string{config.death_fallback_tag}}, {});
    }
    return false;
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

void AttackProcessingHelper::process_pending_attacks(
    Asset& self,
    const AttackProcessingConfig& config) {
    (void)process_attacks(self, self.process_pending_attacks(), config);
}

AttackProcessingSummary AttackProcessingHelper::process_attacks(
    Asset& self,
    const std::vector<animation_update::Attack>& pending_attacks,
    const AttackProcessingConfig& config) {
    AttackProcessingSummary summary{};
    summary.had_pending_attacks = !pending_attacks.empty();
    bool took_damage = false;
    std::optional<SDL_Point> strongest_knockback{};
    for (const auto& attack : pending_attacks) {
        const int applied_damage = std::max(0, attack.payload.damage_amount);
        self.runtime_health = std::max(0, self.runtime_health - applied_damage);
        took_damage = took_damage || applied_damage > 0;
        SDL_Point candidate_knockback{};
        if (!compute_knockback_delta(
                self,
                attack,
                candidate_knockback,
                config.max_knockback_distance,
                config.max_damage_for_knockback)) {
            continue;
        }
        if (config.knockback_scale > 0.0f && std::abs(config.knockback_scale - 1.0f) > kZeroTolerance) {
            candidate_knockback.x = static_cast<int>(std::round(
                static_cast<float>(candidate_knockback.x) * config.knockback_scale));
            candidate_knockback.y = static_cast<int>(std::round(
                static_cast<float>(candidate_knockback.y) * config.knockback_scale));
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

    if (self.runtime_health <= 0) {
        summary.took_damage = took_damage;
        summary.died = true;
        if (!try_play_death_animation(self, config)) {
            self.Delete();
        }
        return summary;
    }

    if (strongest_knockback.has_value()) {
        summary.took_damage = took_damage;
        apply_knockback(self, *strongest_knockback);
        return summary;
    }

    if (!took_damage) {
        return summary;
    }
    summary.took_damage = true;
    if (try_set_animation(self, config.hit_animation_id)) {
        return summary;
    }
    if (config.hit_fallback_animation_id.empty()) {
        return summary;
    }
    (void)try_set_animation(self, config.hit_fallback_animation_id);
    return summary;
}

} // namespace animation_update::custom_controllers
