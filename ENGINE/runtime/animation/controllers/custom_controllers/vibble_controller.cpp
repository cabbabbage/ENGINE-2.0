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
#include "animation/controllers/custom_controllers/player_direction_intent.hpp"
#include "assets/asset/Asset.hpp"
#include "core/AssetsManager.hpp"
#include "utils/input.hpp"
#include <algorithm>
#include <cmath>

namespace {

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
    : CustomAssetController(player) {}


vibble_controller::~vibble_controller() = default;
int vibble_controller::get_dx() const { return dx_; }
int vibble_controller::get_dy() const { return dy_; }

void vibble_controller::movement(const Input& input) {
    dx_ = dy_ = 0;
    Asset* player = self_ptr();
    if (!player || !player->anim_) return;

    const float dt = frame_dt();

    const bool up    = input.isScancodeDown(SDL_SCANCODE_W) || input.isScancodeDown(SDL_SCANCODE_UP);
    const bool down  = input.isScancodeDown(SDL_SCANCODE_S) || input.isScancodeDown(SDL_SCANCODE_DOWN);
    const bool left  = input.isScancodeDown(SDL_SCANCODE_A) || input.isScancodeDown(SDL_SCANCODE_LEFT);
    const bool right = input.isScancodeDown(SDL_SCANCODE_D) || input.isScancodeDown(SDL_SCANCODE_RIGHT);
    const bool sprint = input.isScancodeDown(SDL_SCANCODE_LSHIFT) || input.isScancodeDown(SDL_SCANCODE_RSHIFT);
    const bool dash = input.isScancodeDown(SDL_SCANCODE_SPACE);
    const bool melee = input.isScancodeDown(SDL_SCANCODE_E);


    const int input_x = (right ? 1 : 0) - (left ? 1 : 0);
    const int input_y = (down  ? 1 : 0) - (up    ? 1 : 0);
    const vibble::player_direction::DirectionIntent direction_intent =
        resolve_direction_intent_for_player(player, input_x, input_y);
    const int world_x = direction_intent.world_x;
    const int world_y = direction_intent.world_y;

    if (world_x == 0 && world_y == 0) {
        subpixel_x_ = 0.0f;
        subpixel_y_ = 0.0f;
        player->anim_->move(SDL_Point{ 0, 0 }, animation_update::detail::kDefaultAnimation);
        return;
    }

    const float stride_count = sprint ? kSprintMultiplier : 1.0f;

    if (dash && canDash) {
        Dash();
    }
    if (melee && canMelee) {
        canMelee = false;
        isMeleeing = true;
        meleeCooldownEndTime = std::chrono::steady_clock::now() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<float>(meleeCooldown));
    }

    float speedMultiplier = kWalkSpeed;
    if (isDashing) {
        speedMultiplier *= dashingPower;
    }

    const float velocity_x = static_cast<float>(world_x) * speedMultiplier * stride_count;
    const float velocity_y = static_cast<float>(world_y) * speedMultiplier * stride_count;

    auto consume_axis = [](float& accumulator) -> int {
        int whole = 0;
        if (accumulator >= 1.0f) {
            whole = static_cast<int>(std::floor(accumulator));
            accumulator -= static_cast<float>(whole);
        } else if (accumulator <= -1.0f) {
            whole = static_cast<int>(std::ceil(accumulator));
            accumulator -= static_cast<float>(whole);
        }
        return whole;
    };

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

}

float vibble_controller::frame_dt() const {
    constexpr float kFallbackDt = 1.0f / 60.0f;
    Asset* player = self_ptr();
    if (!player) {
        return kFallbackDt;
    }
    if (Assets* assets = player->get_assets()) {
        const float dt = assets->frame_delta_seconds();
        if (std::isfinite(dt) && dt > 0.0f) {

            return std::min(dt, 0.1f);
        }
    }
    return kFallbackDt;
}

void vibble_controller::on_update(const Input& input) {
    using namespace std::chrono;
    auto now = steady_clock::now();

    ensure_eyes_child();

    if (isDashing && now >= dashEndTime) {
        isDashing = false;
        cooldownEndTime = now + duration_cast<steady_clock::duration>(duration<float>(dashingCooldown));
    }

    if (!canDash && !isDashing && now >= cooldownEndTime) {
        canDash = true;
    }

    if (!canMelee && !isMeleeing && now >= meleeCooldownEndTime) {
        canMelee = true;
    }

    dx_ = dy_ = 0;
    movement(input);
    if (eyes_child_.has_value()) {
        eyes_child_->update();
    }

}

void vibble_controller::ensure_eyes_child() {
    Asset* player = self_ptr();
    if (eyes_child_.has_value() || !player) {
        return;
    }
    if (!player->get_assets()) {
        return;
    }

    eyes_child_.emplace("vibble_eyes");
    eyes_child_->bind("eyes");
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

    auto has_animation = [&animations](const std::string& name) {
        return animations.find(name) != animations.end();
    };

    const std::string forward_anim   = "down";
    const std::string backward_anim  = "up";
    const std::string left_anim      = "left";
    const std::string right_anim     = "right";

    if (sign_x != 0 && sign_y != 0) {
        const std::string vertical_choice = (sign_y > 0) ? backward_anim : forward_anim;
        if (has_animation(vertical_choice)) {
            return vertical_choice;
        }

        const std::string horizontal_choice = (sign_x < 0) ? left_anim : right_anim;
        if (has_animation(horizontal_choice)) {
            return horizontal_choice;
        }
    }

    if (sign_y != 0) {
        const std::string vertical_choice = (sign_y > 0) ? backward_anim : forward_anim;
        if (has_animation(vertical_choice)) {
            return vertical_choice;
        }
    }

    if (sign_x != 0) {
        const std::string horizontal_choice = (sign_x < 0) ? left_anim : right_anim;
        if (has_animation(horizontal_choice)) {
            return horizontal_choice;
        }
    }

    if (has_animation(animation_update::detail::kDefaultAnimation)) {
        return std::string{ animation_update::detail::kDefaultAnimation };
    }

    return std::string{ animation_update::detail::kDefaultAnimation };
}

void vibble_controller::Dash() {
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

    // Get active assets as potential targets
    const auto& active_assets = assets->getActive();

    // Check attacks against all other assets
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

    // Process any attacks that were sent to self
    self.process_pending_attacks();
}


