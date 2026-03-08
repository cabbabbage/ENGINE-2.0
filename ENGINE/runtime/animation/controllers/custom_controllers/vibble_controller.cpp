#include "vibble_controller.hpp"

#include "animation/animation_update.hpp"
#include "animation/attack_validation.hpp"
#include "assets/Asset.hpp"
#include "core/AssetsManager.hpp"
#include "utils/input.hpp"
#include <algorithm>
#include <cmath>

namespace {

float sanitize_scale(float value) {
    return (std::isfinite(value) && value > 0.0f) ? value : 1.0f;
}

animation_update::GeometryContext geometry_for(const Asset& asset) {
    animation_update::GeometryContext context{};
    context.anchor = animation_update::detail::bottom_middle_for(asset, asset.world_xz_point());
    context.scale = sanitize_scale(asset.smoothed_scale());
    context.flipped = asset.flipped;
    context.plane = animation_update::CombatPlane::XY;
    return context;
}

animation_update::CombatantSnapshot snapshot_from_asset(const Asset& asset) {
    animation_update::CombatantSnapshot snapshot;
    snapshot.asset_id = asset.spawn_id.empty() ? (asset.info ? asset.info->name : std::string{}) : asset.spawn_id;
    snapshot.asset_name = asset.info ? asset.info->name : std::string{};
    snapshot.frame = asset.current_animation_frame();
    snapshot.transform = geometry_for(asset);
    return snapshot;
}

constexpr const char* kEyesAnchorName  = "eyes";
constexpr const char* kEyesFollowerId  = "vibble_eyes";

}

vibble_controller::vibble_controller(Asset* player)
    : player_(player) {
    if (player_) {
        binding_helper_ = std::make_unique<AnchorBoundAssetHelper>(player_);
        if (Asset* eyes_child = player_->get_assets() ? player_->get_assets()->spawn_asset(kEyesFollowerId, player_->world_xz_point()) : nullptr) {
            auto anchor = player_->anchor_state(kEyesAnchorName,
                                                anchor_points::GridMaterialization::None,
                                                std::nullopt);
            if (anchor.has_value()) {
                binding_helper_->bind_child_to_anchor(*player_, *eyes_child, anchor.value());
            }
        }
    }
}


vibble_controller::~vibble_controller() = default;
int vibble_controller::get_dx() const { return dx_; }
int vibble_controller::get_dy() const { return dy_; }

void vibble_controller::movement(const Input& input) {
    dx_ = dy_ = 0;
    if (!player_ || !player_->anim_) return;

    const float dt = frame_dt();

    const bool up    = input.isScancodeDown(SDL_SCANCODE_W) || input.isScancodeDown(SDL_SCANCODE_UP);
    const bool down  = input.isScancodeDown(SDL_SCANCODE_S) || input.isScancodeDown(SDL_SCANCODE_DOWN);
    const bool left  = input.isScancodeDown(SDL_SCANCODE_A) || input.isScancodeDown(SDL_SCANCODE_LEFT);
    const bool right = input.isScancodeDown(SDL_SCANCODE_D) || input.isScancodeDown(SDL_SCANCODE_RIGHT);
    const bool sprint = input.isScancodeDown(SDL_SCANCODE_LSHIFT) || input.isScancodeDown(SDL_SCANCODE_RSHIFT);
    const bool dash = input.isScancodeDown(SDL_SCANCODE_SPACE);
    const bool melee = input.isScancodeDown(SDL_SCANCODE_E);


    const int raw_x = (right ? 1 : 0) - (left ? 1 : 0);
    const int raw_y = (down  ? 1 : 0) - (up    ? 1 : 0);

    if (raw_x == 0 && raw_y == 0) {
        subpixel_x_ = 0.0f;
        subpixel_y_ = 0.0f;
        player_->anim_->move(SDL_Point{ 0, 0 }, animation_update::detail::kDefaultAnimation);
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

    const float velocity_x = static_cast<float>(raw_x) * speedMultiplier * stride_count;
    const float velocity_y = static_cast<float>(raw_y) * speedMultiplier * stride_count;

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

    std::string animation_id = animation_for_direction(raw_x, raw_y);
    if (isDashing && player_->info) {

        const auto& animations = player_->info->animations;
        if (animations.find("dash") != animations.end()) {
            animation_id = "dash";
        }
    }

    player_->anim_->move(SDL_Point{ dx_, dy_ }, animation_id);

}

float vibble_controller::frame_dt() const {
    constexpr float kFallbackDt = 1.0f / 60.0f;
    if (!player_) {
        return kFallbackDt;
    }
    if (Assets* assets = player_->get_assets()) {
        const float dt = assets->frame_delta_seconds();
        if (std::isfinite(dt) && dt > 0.0f) {

            return std::min(dt, 0.1f);
        }
    }
    return kFallbackDt;
}

void vibble_controller::update(const Input& input) {
    using namespace std::chrono;
    auto now = steady_clock::now();

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

}

std::string vibble_controller::animation_for_direction(int raw_x, int raw_y) const {
    const int sign_x = (raw_x > 0) - (raw_x < 0);
    const int sign_y = (raw_y > 0) - (raw_y < 0);

    if (sign_x == 0 && sign_y == 0) {
        return std::string{ animation_update::detail::kDefaultAnimation };
    }

    if (!player_ || !player_->info) {
        return std::string{ animation_update::detail::kDefaultAnimation };
    }

    const auto& animations = player_->info->animations;

    auto has_animation = [&animations](const std::string& name) {
        return animations.find(name) != animations.end();
    };

    const std::string forward_anim   = "forward";
    const std::string backward_anim  = "backward";
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

void vibble_controller::process_pending_attacks(Asset& self) {
    using namespace animation_update;

    if (!self.info || !self.current_animation_frame() || self.dead || !self.active) {
        return;
    }

    Assets* assets = self.get_assets();
    if (!assets) {
        return;
    }

    // Create attacker snapshot
    CombatantSnapshot attacker_snapshot = snapshot_from_asset(self);

    // Get active assets as potential targets
    const auto& active_assets = assets->getActive();

    // Check attacks against all other assets
    for (Asset* target : active_assets) {
        if (!target || target == &self || !target->info || !target->current_animation_frame() ||
            target->dead || !target->active) {
            continue;
        }

        CombatantSnapshot target_snapshot = snapshot_from_asset(*target);

        auto attack_opt = AttackValidation::compute_attack_if_hit(
            attacker_snapshot, target_snapshot);

        if (attack_opt.has_value()) {
            target->send_attack(*attack_opt);
        }
    }

    // Process any attacks that were sent to self
    self.process_pending_attacks();
}


