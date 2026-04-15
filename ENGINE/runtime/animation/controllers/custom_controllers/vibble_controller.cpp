// CONTROLLER_META_BEGIN
// Controller: vibble_controller
// Asset: vibble (type: object)
// Available animations [1]:
//   - default
// Generated: 2026-03-17 23:48:57
// CONTROLLER_META_END

#include "vibble_controller.hpp"
#include "animation/animation_update.hpp"
#include "animation/attack_validation.hpp"
#include "animation/controllers/shared/anchor_bound_asset_helper.hpp"
#include "animation/controllers/shared/attack_processing_helper.hpp"
#include "animation/controllers/shared/custom_controller_update_utils.hpp"
#include "animation/controllers/shared/player_direction_intent.hpp"
#include "assets/asset/Asset.hpp"
#include "core/AssetsManager.hpp"
#include "utils/input.hpp"
#include <algorithm>
#include <cmath>
#include <string_view>

namespace {

constexpr std::string_view kMeleeChildAssetName = "vibble_attack_1";
constexpr std::string_view kLegacyMeleeChildAssetName = "vibble_attack";
constexpr std::string_view kMeleeAttackAnimation = "attack";

int consume_axis(float& accumulator) {
    const int whole = static_cast<int>(accumulator);
    if (whole != 0) {
        accumulator -= static_cast<float>(whole);
    }
    return whole;
}

vibble::player_direction::DirectionIntent resolve_direction_intent_for_player(
    const Asset* player,
    int screen_x,
    int screen_y) {
    double camera_right_x = 0.0;
    bool has_camera_basis = false;
    if (player) {
        if (const Assets* assets = player->get_assets()) {
            const world::CameraProjectionParams params = assets->getView().projection_params();
            camera_right_x = params.right_x;
            has_camera_basis = (params.screen_width > 0 && params.screen_height > 0);
        }
    }

    return vibble::player_direction::resolve_direction_intent(
        screen_x,
        screen_y,
        camera_right_x,
        has_camera_basis);
}

Asset* find_child_asset_by_name(Asset& owner, std::string_view child_asset_name) {
    for (Asset* child : owner.children()) {
        if (!child || !child->info) {
            continue;
        }
        if (child->info->name == child_asset_name) {
            return child;
        }
    }
    return nullptr;
}

Asset* resolve_melee_child_asset(Asset& owner) {
    if (Asset* child = find_child_asset_by_name(owner, kMeleeChildAssetName)) {
        return child;
    }
    return find_child_asset_by_name(owner, kLegacyMeleeChildAssetName);
}

void trigger_melee_child_attack_animation(Asset& owner) {
    Asset* melee_child = resolve_melee_child_asset(owner);
    if (!melee_child || !melee_child->anim_) {
        return;
    }

    if (melee_child->info &&
        melee_child->info->animations.find(std::string{kMeleeAttackAnimation}) ==
            melee_child->info->animations.end()) {
        return;
    }

    melee_child->anim_->set_animation(std::string{kMeleeAttackAnimation});
}

}

vibble_controller::vibble_controller(Asset* player)
    : CustomAssetController(player) {}

    

vibble_controller::~vibble_controller() = default;

void vibble_controller::movement(const Input& input) {
    dx_ = dy_ = 0;
    Asset* player = self_ptr();
    if (!player || !player->anim_) {
        return;
    }

    const float dt = player->frame_delta_seconds_clamped();

    const int input_x =
        ((input.isScancodeDown(SDL_SCANCODE_D) || input.isScancodeDown(SDL_SCANCODE_RIGHT)) ? 1 : 0)
        - ((input.isScancodeDown(SDL_SCANCODE_A) || input.isScancodeDown(SDL_SCANCODE_LEFT)) ? 1 : 0);
    const int input_y =
        ((input.isScancodeDown(SDL_SCANCODE_S) || input.isScancodeDown(SDL_SCANCODE_DOWN)) ? 1 : 0)
        - ((input.isScancodeDown(SDL_SCANCODE_W) || input.isScancodeDown(SDL_SCANCODE_UP)) ? 1 : 0);
    const bool sprint =
        input.isScancodeDown(SDL_SCANCODE_LSHIFT) || input.isScancodeDown(SDL_SCANCODE_RSHIFT);
    const bool dash_pressed = input.isScancodeDown(SDL_SCANCODE_SPACE);
    const bool melee_pressed = input.wasPressed(Input::LEFT);

    const vibble::player_direction::DirectionIntent direction_intent =
        resolve_direction_intent_for_player(player, input_x, input_y);
    const int world_x = direction_intent.world_x;
    const int world_y = direction_intent.world_y;

    if (melee_pressed && canMelee) {
        canMelee = false;
        isMeleeing = true;
        meleeCooldownEndTime = std::chrono::steady_clock::now()
                               + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                   std::chrono::duration<float>(meleeCooldown));
        trigger_melee_child_attack_animation(*player);
    }



    const float stride_count = sprint ? kSprintMultiplier : 1.0f;

    if (dash_pressed && canDash) {
        start_dash();
    }

    float speed_multiplier = kWalkSpeed;
    if (isDashing) {
        speed_multiplier *= dashingPower;
    }

    const float velocity_x = static_cast<float>(world_x) * speed_multiplier * stride_count;
    const float velocity_y = static_cast<float>(world_y) * speed_multiplier * stride_count;

    subpixel_x_ += velocity_x * dt;
    subpixel_y_ += velocity_y * dt;

    dx_ = consume_axis(subpixel_x_);
    dy_ = consume_axis(subpixel_y_);

    constexpr float kResidualClamp = 8.0f;
    subpixel_x_ = std::clamp(subpixel_x_, -kResidualClamp, kResidualClamp);
    subpixel_y_ = std::clamp(subpixel_y_, -kResidualClamp, kResidualClamp);

    const bool has_pixel_motion = (dx_ != 0 || dy_ != 0);
    const bool has_movement_intent = (input_x != 0 || input_y != 0);
    const std::string movement_animation =
        animation_for_direction(input_x, input_y);
    const CardinalVector movement_vector = movement_cardinal_vector(input_x, input_y);
    const FacingSelection facing = facing_from_mouse(*player, input, last_facing_animation_);
    if (facing.valid) {
        last_facing_animation_ = facing.animation_id;
    }

    std::string selected_animation = movement_animation;
    bool reverse_selected_animation = false;

    if (!has_movement_intent) {
        const std::string idle_animation = facing.valid
            ? facing.animation_id
            : (!last_facing_animation_.empty()
                   ? last_facing_animation_
                   : std::string{animation_update::detail::kDefaultAnimation});
        last_facing_animation_ = idle_animation;
        apply_idle_facing(idle_animation);
        return;
    }

    if (facing.valid) {
        const int dot = movement_vector.x * facing.vector.x + movement_vector.y * facing.vector.y;
        if (dot == 0) {
            selected_animation = movement_animation;
            reverse_selected_animation = false;
        } else {
            selected_animation = facing.animation_id;
            reverse_selected_animation = (dot < 0);
        }
    } else {
        selected_animation = movement_animation;
        reverse_selected_animation = false;
    }

    if (isDashing && player->info) {
        const auto& animations = player->info->animations;
        if (animations.find("dash") != animations.end()) {
            selected_animation = "dash";
            reverse_selected_animation = false;
        }
    }

    player->anim_->move(SDL_Point{ dx_, dy_ }, selected_animation);
    if (reverse_selected_animation) {
        animation_update::custom_controllers::begin_reverse_current_animation_until_stop(player);
    } else {
        animation_update::custom_controllers::stop_reverse_current_animation(player);
    }

    if (has_pixel_motion) {
        anchor_bound_asset_helper::AnchorBoundAssetHelper::instance().notify_anchor_changed(
            player,
            std::string{});
    }
}

void vibble_controller::on_update(const Input& input) {
    using namespace std::chrono;
    auto now = steady_clock::now();

    Asset* player = self_ptr();

    if (player) {
        bool anchor_heading_changed = false;
        if (const std::optional<SDL_Point> mouse_world = input.mouse_world_position()) {
            anchor_heading_changed =
                player->set_directional_target_world_xz(static_cast<float>(mouse_world->x),
                                                        static_cast<float>(mouse_world->y)) ||
                anchor_heading_changed;
            const float dx = static_cast<float>(mouse_world->x - player->world_x());
            const float dy = static_cast<float>(mouse_world->y - player->world_z());
            constexpr float kMinHeadingVectorLengthSq = 1e-4f;
            const float length_sq = dx * dx + dy * dy;
            if (length_sq > kMinHeadingVectorLengthSq) {
                const float heading_radians = std::atan2(dy, dx);
                anchor_heading_changed =
                    player->set_directional_heading_radians(heading_radians) || anchor_heading_changed;
            }
        }
        if (anchor_heading_changed) {
            anchor_bound_asset_helper::AnchorBoundAssetHelper::instance().notify_anchor_changed(
                player,
                std::string{});
        }
    }

    if (isDashing && now >= dashEndTime) {
        isDashing = false;
        cooldownEndTime = now + duration_cast<steady_clock::duration>(duration<float>(dashingCooldown));
    }

    if (!canDash && !isDashing && now >= cooldownEndTime) {
        canDash = true;
    }

    if (!canMelee && now >= meleeCooldownEndTime) {
        canMelee = true;
        isMeleeing = false;
    }

    movement(input);
}

std::string vibble_controller::animation_for_direction(int screen_x, int screen_y) const {
    const int sign_x = (screen_x > 0) - (screen_x < 0);
    const int sign_y = (screen_y > 0) - (screen_y < 0);

    if (sign_x == 0 && sign_y == 0) {
        return std::string{ animation_update::detail::kDefaultAnimation };
    }

    Asset* player = self_ptr();
    if (!player || !player->info) {
        return std::string{ animation_update::detail::kDefaultAnimation };
    }

    const auto& animations = player->info->animations;
    auto has_animation = [&animations](const char* name) {
        return animations.find(name) != animations.end();
    };

    const char* vertical_choice = sign_y > 0 ? "up" : "down";
    const char* horizontal_choice = sign_x < 0 ? "left" : "right";

    if (sign_x != 0 && sign_y != 0 && has_animation(vertical_choice)) {
        return vertical_choice;
    }
    if (sign_x != 0 && sign_y != 0 && has_animation(horizontal_choice)) {
        return horizontal_choice;
    }
    if (sign_y != 0 && has_animation(vertical_choice)) {
        return vertical_choice;
    }
    if (sign_x != 0 && has_animation(horizontal_choice)) {
        return horizontal_choice;
    }
    return animation_update::detail::kDefaultAnimation;
}

vibble_controller::FacingSelection vibble_controller::facing_from_mouse(
    const Asset& player,
    const Input& input,
    const std::string& fallback_animation) const {
    FacingSelection selection{};
    if (!input.mouse_world_position().has_value()) {
        return selection;
    }

    SDL_FPoint player_screen = SDL_FPoint{
        static_cast<float>(player.world_x()),
        static_cast<float>(player.world_z())
    };
    if (const Assets* assets = player.get_assets()) {
        player_screen = assets->getView().map_to_screen(SDL_Point{player.world_x(), player.world_z()});
    }

    const int delta_x = input.getX() - static_cast<int>(std::lround(player_screen.x));
    const int delta_y = input.getY() - static_cast<int>(std::lround(player_screen.y));

    if (delta_x == 0 && delta_y == 0) {
        selection.animation_id = fallback_animation.empty()
            ? std::string{animation_update::detail::kDefaultAnimation}
            : fallback_animation;
        selection.vector = cardinal_vector_for_animation(selection.animation_id);
        selection.valid = (selection.vector.x != 0 || selection.vector.y != 0);
        return selection;
    }

    const bool horizontal = std::abs(delta_x) >= std::abs(delta_y);
    std::string preferred_animation;
    if (horizontal) {
        preferred_animation = (delta_x < 0) ? "left" : "right";
    } else {
        // Mouse screen-space semantics: below means down, above means up.
        preferred_animation = (delta_y > 0) ? "down" : "up";
    }

    auto has_animation = [&](const std::string& id) {
        return player.info && player.info->animations.find(id) != player.info->animations.end();
    };

    if (has_animation(preferred_animation)) {
        selection.animation_id = preferred_animation;
    } else if (!fallback_animation.empty() && has_animation(fallback_animation)) {
        selection.animation_id = fallback_animation;
    } else {
        selection.animation_id = animation_update::detail::kDefaultAnimation;
    }
    selection.vector = cardinal_vector_for_animation(selection.animation_id);
    selection.valid = (selection.vector.x != 0 || selection.vector.y != 0);
    return selection;
}

vibble_controller::CardinalVector vibble_controller::movement_cardinal_vector(int world_x, int world_y) const {
    const std::string animation_id = animation_for_direction(world_x, world_y);
    return cardinal_vector_for_animation(animation_id);
}

vibble_controller::CardinalVector vibble_controller::cardinal_vector_for_animation(const std::string& animation_id) {
    if (animation_id == "left") {
        return CardinalVector{-1, 0};
    }
    if (animation_id == "right") {
        return CardinalVector{1, 0};
    }
    if (animation_id == "up") {
        return CardinalVector{0, 1};
    }
    if (animation_id == "down") {
        return CardinalVector{0, -1};
    }
    return CardinalVector{0, 0};
}

void vibble_controller::apply_idle_facing(const std::string& animation_id) {
    Asset* player = self_ptr();
    if (!player || !player->anim_) {
        return;
    }
    const std::string resolved_animation =
        animation_id.empty() ? std::string{animation_update::detail::kDefaultAnimation} : animation_id;
    animation_update::custom_controllers::stop_reverse_current_animation(player);
    player->anim_->set_animation(resolved_animation);
}

void vibble_controller::start_dash() {
    if (!canDash) {
        return;
    }

    canDash = false;
    isDashing = true;
    dashEndTime = std::chrono::steady_clock::now()
                  + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                      std::chrono::duration<float>(dashingTime));
}

void vibble_controller::on_process_pending_attacks(Asset& self) {
    using namespace animation_update;

    if (!self.info || !self.current_animation_frame() || self.dead || !self.active) {
        return;
    }

    Assets* assets = self.get_assets();
    if (!assets) {
        return;
    }

    const auto& active_assets = assets->getActive();

    for (Asset* target : active_assets) {
        if (!target || target == &self || !target->info || !target->current_animation_frame() ||
            target->dead || !target->active) {
            continue;
        }

        auto attack_opt = AttackValidation::compute_attack_if_hit(self, *target);

        if (attack_opt.has_value()) {
            target->send_attack(*attack_opt);
        }
    }

    const auto pending_attacks = self.process_pending_attacks();
    if (pending_attacks.empty()) {
        return;
    }

    std::optional<SDL_Point> bump_delta;
    auto consider_knockback = [&](const animation_update::Attack& attack) {
        SDL_Point candidate_delta{};
        if (!animation_update::custom_controllers::AttackProcessingHelper::compute_knockback_delta(self, attack, candidate_delta)) {
            return;
        }
        if (!bump_delta.has_value()) {
            bump_delta = candidate_delta;
            return;
        }
        const int candidate_sq = candidate_delta.x * candidate_delta.x + candidate_delta.y * candidate_delta.y;
        const int current_sq = bump_delta->x * bump_delta->x + bump_delta->y * bump_delta->y;
        if (candidate_sq > current_sq) {
            bump_delta = candidate_delta;
        }
    };

    for (const auto& attack : pending_attacks) {
        self.runtime_health -= attack.damage_amount;
        if (attack.attacker_asset_name == "spider") {
            consider_knockback(attack);
        }
    }

    if (self.runtime_health < 0) {
        if (!animation_update::custom_controllers::AttackProcessingHelper::try_play_death_animation(self)) {
            self.Delete();
        }
        return;
    }

    if (bump_delta.has_value()) {
        animation_update::custom_controllers::AttackProcessingHelper::apply_knockback(self, *bump_delta);
    }
}
