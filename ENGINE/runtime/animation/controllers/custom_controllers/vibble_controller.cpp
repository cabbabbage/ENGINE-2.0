// CONTROLLER_META_BEGIN
// Controller: vibble_controller
// Asset: vibble (type: object)
// Available animations [1]:
//   - default
// Generated: 2026-03-17 23:48:57
// CONTROLLER_META_END

#include "vibble_controller.hpp"
#include <iostream>
#include "animation/animation_update.hpp"
#include "animation/attack_validation.hpp"
#include "animation/controllers/shared/anchor_bound_asset_helper.hpp"
#include "animation/controllers/shared/player_direction_intent.hpp"
#include "assets/asset/Asset.hpp"
#include "core/AssetsManager.hpp"
#include "utils/input.hpp"
#include <algorithm>

namespace {

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

}

vibble_controller::vibble_controller(Asset* player)
    : CustomAssetController(player) {
        
        
        if (player){
            if(!eyes_child_.has_value()) {
                eyes_child_.emplace(*player, "vibble_eyes");
                eyes_child_->bind("eyes");
                child_assets_.push_back(&*eyes_child_);
            }
            if(!hat_child_.has_value()) {
                hat_child_.emplace(*player, "vibble_hat");
                hat_child_->bind("hat");
                child_assets_.push_back(&*hat_child_);
            }
            if (!mouth_child_.has_value()) {
                mouth_child_.emplace(*player, "vibble_mouth");
                mouth_child_->bind("mouth");
                child_assets_.push_back(&*mouth_child_);
            }
            if (!neck_child_.has_value()) {
                neck_child_.emplace(*player, "vibble_neck");
                neck_child_->bind("neck");
                child_assets_.push_back(&*neck_child_);   
            }
            if (!weapon_child_.has_value()) {
                weapon_child_.emplace(*player, "vibble_attack_1");
                weapon_child_->bind("weapon");
               // weapon_child_->hide();
                child_assets_.push_back(&*weapon_child_);   
            }
        }

    }

    

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
    const bool melee_pressed = input.wasScancodePressed(SDL_SCANCODE_E);

    const vibble::player_direction::DirectionIntent direction_intent =
        resolve_direction_intent_for_player(player, input_x, input_y);
    const int world_x = direction_intent.world_x;
    const int world_y = direction_intent.world_y;

    if (melee_pressed) {
        std::cout << "Melee attack initiated!" << std::endl;
        canMelee = false;
        isMeleeing = true;
        meleeCooldownEndTime = std::chrono::steady_clock::now()
                               + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                   std::chrono::duration<float>(meleeCooldown));
        if (weapon_child_.has_value()) {
            weapon_child_->set_animation("attack");
        }
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

    // Keep animation-facing/anchor selection aligned with actual world movement.
    std::string animation_id = animation_for_direction(
        direction_intent.world_x,
        direction_intent.world_y);
    if (isDashing && player->info) {
        const auto& animations = player->info->animations;
        if (animations.find("dash") != animations.end()) {
            animation_id = "dash";
        }
    }

    player->anim_->move(SDL_Point{ dx_, dy_ }, animation_id);

    if (dx_ != 0 || dy_ != 0) {
        anchor_bound_asset_helper::AnchorBoundAssetHelper::instance().notify_anchor_changed(
            player,
            std::string{});
    }
}

void vibble_controller::on_update(const Input& input) {
    using namespace std::chrono;
    auto now = steady_clock::now();

    Asset* player = self_ptr();

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

    CustomAssetController::on_process_pending_attacks(self);
}
