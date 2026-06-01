#include "fly_controller.hpp"

#include "animation/animation_update.hpp"
#include "animation/attack.hpp"
#include "assets/asset/Asset.hpp"
#include "core/AssetsManager.hpp"
#include "gameplay/map_generation/room.hpp"
#include "utils/input.hpp"

#include <cmath>

namespace {

constexpr int kAggressiveContactDamageCooldownFrames = 18;

axis::WorldPos orbit_target_for_angle(const axis::WorldPos& center,
                                      float angle,
                                      int radius,
                                      int vertical_amp) {
    const float cos_a = std::cos(angle);
    const float sin_a = std::sin(angle);
    return axis::WorldPos{
        center.x + static_cast<int>(std::lround(cos_a * static_cast<float>(radius))),
        center.y + static_cast<int>(std::lround(std::sin(angle * 0.5f) * static_cast<float>(vertical_amp))),
        center.z + static_cast<int>(std::lround(sin_a * static_cast<float>(radius)))};
}

} // namespace

fly_controller::fly_controller(Asset* self)
    : custom_controller_api::CustomControllerBase(self) {
    Asset* owner = controller_self();
    if (owner) {
        owner->needs_target = true;
    }
}

void fly_controller::on_update(const Input& in) {
    (void)in;
    Asset* self = controller_self();
    if (!self) {
        return;
    }

    const auto& ctx = controller_game_context();
    if (orbiting && ctx.fly_orbit_point.valid) {
        const axis::WorldPos center{
            ctx.fly_orbit_point.world_xz.x,
            ctx.fly_orbit_point.world_y,
            ctx.fly_orbit_point.world_xz.y};

        const int radius = ctx.room_flies_aggressive() ? 30 : 80;
        const int vertical_amp = ctx.room_flies_aggressive() ? 20 : 10;
        orbit_angle_radians_ += ctx.room_flies_aggressive() ? 0.22f : 0.08f;

        custom_controller_api::MovementConfig move_cfg{};
        move_cfg.visit_threshold_px = ctx.fly_orbit_behavior_config().visit_threshold_px;
        move_cfg.override_non_locked = ctx.fly_orbit_behavior_config().override_non_locked;
        move_cfg.resolution_layer = ctx.fly_orbit_point.grid_resolution;

        (void)move_toward(orbit_target_for_angle(center,
                                                 orbit_angle_radians_,
                                                 radius,
                                                 vertical_amp),
                          ctx.room_flies_aggressive() ? 24 : 10,
                          move_cfg);

        if (contact_damage_cooldown_frames_ > 0) {
            --contact_damage_cooldown_frames_;
        }
        if (ctx.room_flies_aggressive() && contact_damage_cooldown_frames_ == 0) {
            if (apply_attack_hits_to_active_targets()) {
                contact_damage_cooldown_frames_ = kAggressiveContactDamageCooldownFrames;
            }
        }
        return;
    }

    if (self->anim_) {
        self->anim_->set_animation(animation_update::detail::kDefaultAnimation);
    }
}

void fly_controller::on_process_pending_attacks(Asset& self_ref) {
    custom_controller_api::CustomControllerBase::on_process_pending_attacks(self_ref);
}

void fly_controller::on_attack(const animation_update::Attack& attack) {
    Asset* self = controller_self();
    if (!self) {
        return;
    }
    if (attack.attacker_asset_name == "vibble_attack_1" || attack.attacker_asset_name == "vibble") {
        if (Assets* owner_assets = self->get_assets()) {
            std::string room_name = self->owning_room_name();
            if (room_name.empty()) {
                if (Room* room = owner_assets->current_room()) {
                    room_name = room->room_name;
                }
            }
            owner_assets->mutable_game_context().set_room_fly_aggression(room_name, 20.0f);
        }

        if (self->anim_) {
            self->anim_->cancel_all_movement();
            self->anim_->set_animation(animation_update::detail::kDefaultAnimation);
        }
        self->move_to_world_position(self->world_x(), -10, self->world_z(), self->grid_resolution);
        orbiting = false;
    }
}
