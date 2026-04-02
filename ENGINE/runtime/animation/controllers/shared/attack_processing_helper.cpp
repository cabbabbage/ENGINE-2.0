#include  animation/controllers/shared/attack_processing_helper.hpp

#include animation/animation_update.hpp
#include assets/asset/Asset.hpp

#include <algorithm>
#include <cmath>
#include <limits>

namespace animation_update::custom_controllers {

namespace {

constexpr float kZeroTolerance = 1e-4f;
constexpr char kDieAnimationId[] = die;
constexpr char kBreakAnimationId[] = break;

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
    if (max_distance <= 0.0f || max_damage <= 0) {
        return false;
    }

    const float clamped_damage = std::clamp(static_cast<float>(attack.damage_amount), 0.0f, static_cast<float>(max_damage));
    if (clamped_damage <= 0.0f) {
        return false;
    }

    const float damage_ratio = clamped_damage / static_cast<float>(max_damage);
    const float travel_distance = damage_ratio * max_distance;
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
    for (const auto& attack : pending_attacks) {
        self.runtime_health -= attack.damage_amount;
    }

    if (self.runtime_health < 0) {
        if (!try_play_death_animation(self)) {
            self.Delete();
        }
    }
}

} // namespace animation_update::custom_controllers
