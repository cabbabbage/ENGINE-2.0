#include "vibble_controller.hpp"
#include "animation/attack.hpp"
#include "animation/animation_update.hpp"
#include "animation/controllers/shared/anchor_bound_asset_helper.hpp"
#include "animation/controllers/shared/attack_processing_helper.hpp"
#include "animation/controllers/shared/custom_controller_api.hpp"
#include "animation/controllers/shared/player_direction_intent.hpp"
#include "assets/asset/Asset.hpp"
#include "core/AssetsManager.hpp"
#include "core/game_runtime_context.hpp"
#include "utils/frame_stats_recorder.hpp"
#include "utils/input.hpp"
#include "utils/log.hpp"
#include "utils/range_util.hpp"
#include "utils/string_utils.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace {

constexpr std::string_view kMeleeChildAssetName = "vibble_attack_1";
constexpr std::string_view kLegacyMeleeChildAssetName = "vibble_attack";
constexpr std::string_view kMeleeAttackAnimation = "attack";
constexpr std::string_view kMeleeAnchorName = "melee";
constexpr std::string_view kHandAnchorName = "hand";
constexpr std::string_view kGunAssetName = "gun";
constexpr std::string_view kSpiderEggAssetName = "spider_egg";
constexpr std::string_view kInteractableTag = "interactable";
constexpr std::string_view kCanCarryTag = "can_carry";
constexpr int kInteractRadiusPx = 150;
constexpr float kYawSensitivity = 0.60f;
constexpr float kPitchSensitivity = 0.80f;
constexpr float kPitchMinDegrees = -45.0f;
constexpr float kPitchMaxDegrees = 45.0f;

std::string normalize_tag_token(std::string_view value) {
    return vibble::strings::to_lower_copy(vibble::strings::trim_copy(std::string{value}));
}

bool normalized_tokens_equal(std::string_view left, std::string_view right) {
    return normalize_tag_token(left) == normalize_tag_token(right);
}

std::vector<std::string> normalize_tag_list(const std::vector<std::string>& values) {
    std::vector<std::string> normalized;
    normalized.reserve(values.size());
    std::unordered_set<std::string> seen;
    seen.reserve(values.size());
    for (const std::string& raw : values) {
        std::string token = vibble::strings::to_lower_copy(vibble::strings::trim_copy(raw));
        if (token.empty()) {
            continue;
        }
        if (seen.insert(token).second) {
            normalized.push_back(std::move(token));
        }
    }
    return normalized;
}

float wrap_degrees_180(float degrees) {
    float wrapped = std::fmod(degrees + 180.0f, 360.0f);
    if (wrapped < 0.0f) {
        wrapped += 360.0f;
    }
    return wrapped - 180.0f;
}

std::optional<float> yaw_degrees_from_camera_forward_horizontal(const Asset& player) {
    const Assets* assets = player.get_assets();
    if (!assets) {
        return std::nullopt;
    }

    const world::CameraProjectionParams params = assets->getView().projection_params();
    const double forward_x = params.forward_x;
    const double forward_z = params.forward_z;
    if (!std::isfinite(forward_x) || !std::isfinite(forward_z)) {
        return std::nullopt;
    }

    constexpr double kMinHorizontalLengthSq = 1e-8;
    const double horizontal_length_sq = forward_x * forward_x + forward_z * forward_z;
    if (horizontal_length_sq <= kMinHorizontalLengthSq) {
        return std::nullopt;
    }

    const double yaw_radians = std::atan2(forward_z, forward_x);
    const double yaw_degrees = yaw_radians * (180.0 / 3.14159265358979323846);
    return wrap_degrees_180(static_cast<float>(yaw_degrees));
}

int consume_axis(float& accumulator) {
    const int whole = static_cast<int>(accumulator);
    if (whole != 0) {
        accumulator -= static_cast<float>(whole);
    }
    return whole;
}

SDL_Point clamp_delta_magnitude(int dx, int dy, float max_magnitude) {
    if (max_magnitude <= 0.0f) {
        return SDL_Point{0, 0};
    }
    const double magnitude = std::sqrt(static_cast<double>(dx) * static_cast<double>(dx) +
                                       static_cast<double>(dy) * static_cast<double>(dy));
    if (magnitude <= static_cast<double>(max_magnitude) || magnitude <= 1.0e-6) {
        return SDL_Point{dx, dy};
    }

    const double scale = static_cast<double>(max_magnitude) / magnitude;
    SDL_Point clamped{
        static_cast<int>(std::round(static_cast<double>(dx) * scale)),
        static_cast<int>(std::round(static_cast<double>(dy) * scale))
    };
    if (clamped.x == 0 && dx != 0) {
        clamped.x = (dx > 0) ? 1 : -1;
    }
    if (clamped.y == 0 && dy != 0) {
        clamped.y = (dy > 0) ? 1 : -1;
    }
    return clamped;
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

    std::vector<std::string> required_tags;
    std::vector<std::string> excluded_tags;
    const auto melee_anchor = owner.anchor_state(std::string{kMeleeAnchorName},
                                                 anchor_points::GridMaterialization::None,
                                                 Asset::AnchorResolveMode::ForceRecompute);
    if (melee_anchor.has_value()) {
        required_tags = normalize_tag_list(melee_anchor->tags);
        excluded_tags = normalize_tag_list(melee_anchor->anti_tags);
    }

    const std::string attack_tag = std::string{kMeleeAttackAnimation};
    if (std::find(required_tags.begin(), required_tags.end(), attack_tag) == required_tags.end()) {
        required_tags.push_back(attack_tag);
    }
    excluded_tags.erase(
        std::remove(excluded_tags.begin(), excluded_tags.end(), attack_tag),
        excluded_tags.end());

    (void)melee_child->anim_->set_animation_by_tags(required_tags, excluded_tags);
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
    const Assets* owner_assets = player->get_assets();
    const bool dev_mode = owner_assets && owner_assets->is_dev_mode();
    // Aim system split by mode:
    // - Dev mode: cursor-projected world target (mouse raycast path) drives facing.
    // - Normal mode: FPS-like yaw/pitch accumulators drive facing/aim.
    //
    // When transitioning into normal mode, re-seed yaw from camera-facing (or last facing)
    // and reset pitch so we do not reuse stale pre-dev pitch state.
    if (!dev_mode && !normal_mode_active_) {
        if (const std::optional<float> camera_yaw = yaw_degrees_from_camera_forward_horizontal(*player)) {
            yaw_angle_degrees_ = *camera_yaw;
        } else {
            yaw_angle_degrees_ = yaw_degrees_for_animation(last_facing_animation_);
        }
        pitch_angle_degrees_ = 0.0f;
        normal_mode_active_ = true;
    } else if (dev_mode) {
        // Leaving normal-mode aim loop; while in dev mode we must not update yaw/pitch
        // from mouse deltas because dev mode is cursor-world-target driven.
        normal_mode_active_ = false;
    }
    if (!dev_mode) {
        yaw_angle_degrees_ = wrap_degrees_180(yaw_angle_degrees_ + static_cast<float>(input.getDX()) * kYawSensitivity);
        // Mouse-up should tilt aim upward, so Y delta decreases pitch angle.
        pitch_angle_degrees_ = std::clamp(
            pitch_angle_degrees_ - static_cast<float>(input.getDY()) * kPitchSensitivity,
            kPitchMinDegrees,
            kPitchMaxDegrees);
    }

    const int input_x =
        ((input.isScancodeDown(SDL_SCANCODE_D) || input.isScancodeDown(SDL_SCANCODE_RIGHT)) ? 1 : 0)
        - ((input.isScancodeDown(SDL_SCANCODE_A) || input.isScancodeDown(SDL_SCANCODE_LEFT)) ? 1 : 0);
    const int input_y =
        ((input.isScancodeDown(SDL_SCANCODE_S) || input.isScancodeDown(SDL_SCANCODE_DOWN)) ? 1 : 0)
        - ((input.isScancodeDown(SDL_SCANCODE_W) || input.isScancodeDown(SDL_SCANCODE_UP)) ? 1 : 0);
    const bool sprint =
        input.isScancodeDown(SDL_SCANCODE_LSHIFT) || input.isScancodeDown(SDL_SCANCODE_RSHIFT);
    const bool dash_pressed = input.isScancodeDown(SDL_SCANCODE_SPACE);
    const bool interact_pressed = input.isScancodeDown(SDL_SCANCODE_E);
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

    if (interact_pressed && !isInteracting) {
        isInteracting = true;
        interactFrames = 0;
    }

    if (isInteracting) {
        if (interact_pressed) {
            interactFrames++;
        } else {
            const int held_frames = interactFrames;
            isInteracting = false;
            interactFrames = 0;
            process_interact(input, held_frames);
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

    const int raw_dx = dx_;
    const int raw_dy = dy_;
    constexpr float kMaxNormalMovePixelsPerFrame = 48.0f;
    constexpr float kMaxDashMovePixelsPerFrame = 96.0f;
    const SDL_Point clamped_delta = clamp_delta_magnitude(
        dx_,
        dy_,
        isDashing ? kMaxDashMovePixelsPerFrame : kMaxNormalMovePixelsPerFrame);
    dx_ = clamped_delta.x;
    dy_ = clamped_delta.y;

    auto& frame_stats = runtime_stats::FrameStatsRecorder::instance();
    frame_stats.set("movement.player_input_delta_raw_x", raw_dx);
    frame_stats.set("movement.player_input_delta_raw_y", raw_dy);
    frame_stats.set("movement.player_input_delta_clamped_x", dx_);
    frame_stats.set("movement.player_input_delta_clamped_y", dy_);
    frame_stats.set("movement.player_input_delta_clamped", raw_dx != dx_ || raw_dy != dy_);
    frame_stats.set("movement.player_dash_active", isDashing);
    frame_stats.set("movement.player_input_x", input_x);
    frame_stats.set("movement.player_input_y", input_y);
    frame_stats.set("movement.player_world_intent_x", world_x);
    frame_stats.set("movement.player_world_intent_y", world_y);

    constexpr float kResidualClamp = 8.0f;
    subpixel_x_ = std::clamp(subpixel_x_, -kResidualClamp, kResidualClamp);
    subpixel_y_ = std::clamp(subpixel_y_, -kResidualClamp, kResidualClamp);

    const bool has_pixel_motion = (dx_ != 0 || dy_ != 0);
    const bool has_movement_intent = (input_x != 0 || input_y != 0);
    sprint_intent_active_ = sprint && has_movement_intent;
    frame_stats.set("movement.player_has_intent", has_movement_intent);
    const std::string movement_animation =
        animation_for_direction(input_x, input_y);
    const CardinalVector movement_vector = movement_cardinal_vector(input_x, input_y);
    const FacingSelection facing = dev_mode
        ? facing_from_mouse(*player, input, last_facing_animation_)
        : facing_from_yaw(yaw_angle_degrees_, last_facing_animation_);
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


    if (reverse_selected_animation) {
        custom_controller_api::begin_reverse_current_animation_until_stop(player);
    } else {
        custom_controller_api::stop_reverse_current_animation(player);
    }

    if (has_pixel_motion) {
        anchor_bound_asset_helper::AnchorBoundAssetHelper::instance().notify_anchor_changed(
            player,
            std::string{});
    }
    player->anim_->move(SDL_Point{ dx_, dy_ }, selected_animation);
}

void vibble_controller::on_update(const Input& input) {
    ensure_hand_defaults();
    movement(input);
    update_world_carried_asset_pose();
    using namespace std::chrono;
    auto now = steady_clock::now();

    Asset* player = self_ptr();

    if (player) {
        const Assets* owner_assets = player->get_assets();
        const bool dev_mode = owner_assets && owner_assets->is_dev_mode();
        bool anchor_heading_changed = false;
        if (dev_mode) {
            // Dev mode uses cursor -> world projection as the sole aim source.
            // Keep this path isolated from normal-mode yaw/pitch aiming.
            player->clear_directional_pitch_radians();
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
        } else {
            // Normal mode uses yaw/pitch accumulators only (no cursor raycast/aim path).
            const float heading_radians =
                wrap_degrees_180(yaw_angle_degrees_) * static_cast<float>(3.14159265358979323846 / 180.0);
            const float pitch_radians =
                pitch_angle_degrees_ * static_cast<float>(3.14159265358979323846 / 180.0);
            const float horizontal_scale = std::cos(pitch_radians);
            anchor_heading_changed =
                player->set_directional_heading_radians(heading_radians) || anchor_heading_changed;
            anchor_heading_changed =
                player->set_directional_pitch_radians(pitch_radians) || anchor_heading_changed;
            constexpr float kDirectionalTargetDistancePx = 128.0f;
            const float target_x = static_cast<float>(player->world_x()) +
                                   std::cos(heading_radians) * horizontal_scale * kDirectionalTargetDistancePx;
            const float target_y = static_cast<float>(player->world_z()) +
                                   std::sin(heading_radians) * horizontal_scale * kDirectionalTargetDistancePx;
            anchor_heading_changed =
                player->set_directional_target_world_xz(target_x, target_y) || anchor_heading_changed;
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

    const bool sprint_or_dash_active = sprint_intent_active_ || isDashing;
    constexpr float kEggDisturbanceIntervalSeconds = 0.2f;
    if (sprint_or_dash_active &&
        (!was_sprint_or_dash_active_ || now >= nextEggDisturbanceTime_)) {
        handle_sprint_dash_egg_disturbance(input);
        nextEggDisturbanceTime_ =
            now + duration_cast<steady_clock::duration>(duration<float>(kEggDisturbanceIntervalSeconds));
    }
    if (!sprint_or_dash_active) {
        nextEggDisturbanceTime_ = now;
    }
    was_sprint_or_dash_active_ = sprint_or_dash_active;

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

vibble_controller::FacingSelection vibble_controller::facing_from_yaw(
    float yaw_angle_degrees,
    const std::string& fallback_animation) const {
    FacingSelection selection{};
    selection.animation_id = animation_for_yaw_degrees(yaw_angle_degrees);
    if (selection.animation_id == animation_update::detail::kDefaultAnimation && !fallback_animation.empty()) {
        selection.animation_id = fallback_animation;
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

float vibble_controller::yaw_degrees_for_animation(const std::string& animation_id) {
    if (animation_id == "right") return 0.0f;
    if (animation_id == "up") return 90.0f;
    if (animation_id == "left") return 180.0f;
    if (animation_id == "down") return -90.0f;
    return 0.0f;
}

std::string vibble_controller::animation_for_yaw_degrees(float yaw_angle_degrees) {
    const float yaw = wrap_degrees_180(yaw_angle_degrees);
    if (yaw >= -45.0f && yaw < 45.0f) return "right";
    if (yaw >= 45.0f && yaw < 135.0f) return "up";
    if (yaw >= -135.0f && yaw < -45.0f) return "down";
    return "left";
}

void vibble_controller::apply_idle_facing(const std::string& animation_id) {
    Asset* player = self_ptr();
    if (!player || !player->anim_) {
        return;
    }
    const std::string resolved_animation =
        animation_id.empty() ? std::string{animation_update::detail::kDefaultAnimation} : animation_id;
    custom_controller_api::stop_reverse_current_animation(player);
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
    CustomAssetController::on_process_pending_attacks(self);
}

void vibble_controller::on_hit(const animation_update::Attack& attack) {
    Asset* player = self_ptr();
    const int health_after = player ? player->runtime_health : 0;
    const int damage = std::max(attack.payload.damage_amount, attack.damage_amount);
    const int starting_health =
        (player && player->info) ? std::max(1, player->info->starting_health) : std::max(1, health_after + damage);

    if (runtime::context::GameRuntimeContext* runtime_ctx = mutable_runtime_game_context()) {
        runtime_ctx->emit_player_damage_pulse(damage, health_after, starting_health);
    }

    auto& frame_stats = runtime_stats::FrameStatsRecorder::instance();
    frame_stats.set("combat.vibble_took_damage", true);
    frame_stats.set("combat.vibble_last_damage", damage);
    frame_stats.set("combat.vibble_health_after_damage", health_after);
    frame_stats.set("combat.vibble_last_attacker", attack.attacker_asset_name);

    std::ostringstream oss;
    oss << "[Combat] Vibble took damage attacker='" << attack.attacker_asset_name
        << "' type='" << attack.attack_type
        << "' damage=" << damage
        << " health_after=" << health_after;
    vibble::log::info(oss.str());
}

void vibble_controller::on_death() {
    vibble::log::info("[Combat] Vibble died from accumulated damage; forcing dev mode");
    if (Assets* owner_assets = assets()) {
        owner_assets->set_dev_mode(true);
    }
}

bool vibble_controller::has_tag(const Asset& asset, std::string_view tag) const {
    if (!asset.info) {
        return false;
    }
    const std::string expected = normalize_tag_token(tag);
    if (expected.empty()) {
        return false;
    }
    if (asset.info->has_tag(expected)) {
        return true;
    }
    for (const std::string& existing : asset.info->tags) {
        if (normalize_tag_token(existing) == expected) {
            return true;
        }
    }
    return false;
}

Asset* vibble_controller::find_closest_tagged_asset(std::string_view tag, int radius_px) const {
    Asset* player = self_ptr();
    Assets* owner_assets = assets();
    if (!player || !owner_assets || radius_px <= 0) {
        return nullptr;
    }

    const long long radius_sq = static_cast<long long>(radius_px) * static_cast<long long>(radius_px);
    long long best_dist_sq = std::numeric_limits<long long>::max();
    Asset* best = nullptr;
    for (Asset* candidate : owner_assets->getActive()) {
        if (!candidate || candidate == player || candidate->dead || !candidate->active || !candidate->info) {
            continue;
        }
        if (candidate == carried_world_asset_) {
            continue;
        }
        if (!has_tag(*candidate, tag)) {
            continue;
        }
        const long long dist_sq = Range::distance_sq(player, candidate);
        if (dist_sq > radius_sq || dist_sq >= best_dist_sq) {
            continue;
        }
        best_dist_sq = dist_sq;
        best = candidate;
    }
    return best;
}

bool vibble_controller::is_carrying_non_gun() const {
    if (carried_world_asset_ && !carried_world_asset_->dead) {
        return !normalized_tokens_equal(carried_asset_name_, kGunAssetName);
    }
    if (!carried_child_.has_value()) {
        return false;
    }
    if (!carried_child_->get_asset()) {
        return false;
    }
    return !normalized_tokens_equal(carried_asset_name_, kGunAssetName);
}

void vibble_controller::ensure_hand_defaults() {
    Asset* player = self_ptr();
    if (!player) {
        return;
    }

    if (!is_carrying_non_gun()) {
        if (!gun_child_.has_value()) {
            gun_child_.emplace(*player, std::string{kGunAssetName});
        }
        gun_child_->bind(std::string{kHandAnchorName});
        gun_child_->unhide();
        return;
    }

    if (gun_child_.has_value()) {
        gun_child_->hide();
    }
}

void vibble_controller::update_world_carried_asset_pose() {
    Asset* player = self_ptr();
    if (!player || !carried_world_asset_ || carried_world_asset_->dead) {
        carried_world_asset_ = nullptr;
        return;
    }

    const auto hand_anchor = player->anchor_state(std::string{kHandAnchorName},
                                                  anchor_points::GridMaterialization::None,
                                                  Asset::AnchorResolveMode::ForceRecompute);
    if (!hand_anchor.has_value()) {
        return;
    }

    carried_world_asset_->set_anchor_hidden(false);
    carried_world_asset_->set_hidden(false);
    carried_world_asset_->active = true;

    int target_layer = hand_anchor->resolution_layer;
    if (target_layer < 0) {
        target_layer = carried_world_asset_->grid_resolution;
    }
    // Anchor contract: world_quantized_px = {world_x, world_y}, and world_z is the depth axis.
    carried_world_asset_->move_to_world_position(hand_anchor->world_quantized_px.x,
                                                 hand_anchor->world_quantized_px.y,
                                                 hand_anchor->world_z,
                                                 target_layer);
    anchor_bound_asset_helper::AnchorBoundAssetHelper::instance().notify_anchor_changed(
        carried_world_asset_,
        std::string{});
}

OrphanImpulse vibble_controller::build_throw_impulse(const Asset& player,
                                                     const Input& input,
                                                     int held_frames) const {
    float dir_x = 0.0f;
    float dir_z = 0.0f;
    float vertical_scale = 1.0f;
    const Assets* owner_assets = player.get_assets();
    const bool dev_mode = owner_assets && owner_assets->is_dev_mode();
    if (dev_mode) {
        if (const std::optional<SDL_Point> mouse_world = input.mouse_world_position()) {
            dir_x = static_cast<float>(mouse_world->x - player.world_x());
            dir_z = static_cast<float>(mouse_world->y - player.world_z());
        }
        if (std::abs(dir_x) <= 1e-4f && std::abs(dir_z) <= 1e-4f) {
            const CardinalVector facing = cardinal_vector_for_animation(last_facing_animation_);
            dir_x = static_cast<float>(facing.x);
            dir_z = static_cast<float>(facing.y);
        }
    } else {
        const float heading_radians =
            wrap_degrees_180(yaw_angle_degrees_) * static_cast<float>(3.14159265358979323846 / 180.0);
        const float pitch_radians = pitch_angle_degrees_ * static_cast<float>(3.14159265358979323846 / 180.0);
        const float horizontal_scale = std::cos(pitch_radians);
        dir_x = std::cos(heading_radians) * horizontal_scale;
        dir_z = std::sin(heading_radians) * horizontal_scale;
        vertical_scale = std::sin(pitch_radians);
    }
    if (std::abs(dir_x) <= 1e-4f && std::abs(dir_z) <= 1e-4f) {
        dir_x = 1.0f;
        dir_z = 0.0f;
    }
    const float length = std::sqrt(dir_x * dir_x + dir_z * dir_z);
    if (length > 1e-4f) {
        dir_x /= length;
        dir_z /= length;
    }

    const int clamped_hold = std::clamp(held_frames, 1, 120);
    const float force = 350.0f + static_cast<float>(clamped_hold) * 14.0f;
    const float upward_force = 220.0f + static_cast<float>(clamped_hold) * 10.0f;
    return OrphanImpulse{dir_x, dir_z, force, upward_force * vertical_scale};
}

void vibble_controller::drop_carried_asset(const Input& input, int held_frames) {
    Asset* player = self_ptr();
    if (!player) {
        return;
    }
    if (!is_carrying_non_gun()) {
        return;
    }

    const OrphanImpulse impulse = build_throw_impulse(*player, input, held_frames);
    if (carried_world_asset_ && !carried_world_asset_->dead) {
        carried_world_asset_->set_anchor_hidden(false);
        carried_world_asset_->set_hidden(false);
        carried_world_asset_->active = true;
        carried_world_asset_->notify_orphaned(player, impulse);
        carried_world_asset_ = nullptr;
        carried_asset_name_.clear();
        ensure_hand_defaults();
        return;
    }

    if (!carried_child_.has_value()) {
        return;
    }

    (void)carried_child_->orphan(impulse);
    carried_child_.reset();
    carried_asset_name_.clear();
    ensure_hand_defaults();
}

void vibble_controller::drop_held_spider_egg_forced(const Input& input) {
    Asset* player = self_ptr();
    if (!player || !carried_world_asset_ || carried_world_asset_->dead) {
        return;
    }
    if (!is_spider_egg_asset(carried_world_asset_)) {
        return;
    }

    const OrphanImpulse impulse = build_throw_impulse(*player, input, 1);
    carried_world_asset_->set_anchor_hidden(false);
    carried_world_asset_->set_hidden(false);
    carried_world_asset_->active = true;
    carried_world_asset_->notify_orphaned(player, impulse);
    carried_world_asset_ = nullptr;
    carried_asset_name_.clear();
    ensure_hand_defaults();
}

void vibble_controller::pickup_asset(Asset& player, Asset& target) {
    if (!target.info) {
        return;
    }
    if (&target == &player) {
        return;
    }

    if (Assets* owner_assets = assets()) {
        for (Asset* candidate_owner : owner_assets->getActive()) {
            if (!candidate_owner || candidate_owner == &target) {
                continue;
            }
            if (candidate_owner->has_child(&target)) {
                candidate_owner->remove_child(&target);
            }
        }
    }
    anchor_bound_asset_helper::AnchorBoundAssetHelper::instance().unregister_child(&target);
    target.clear_anchor_perspective_override();
    target.clear_anchor_sprite_transform_override();
    target.clear_render_anchor_offset();
    target.set_anchor_hidden(false);
    target.set_hidden(false);
    target.active = true;
    if (Assets* owner_assets = assets()) {
        owner_assets->mark_active_assets_dirty();
    }

    carried_asset_name_ = target.info->name;
    carried_child_.reset();
    carried_world_asset_ = &target;
    update_world_carried_asset_pose();
    if (gun_child_.has_value()) {
        gun_child_->hide();
    }
}

void vibble_controller::process_interact(const Input& input, int held_frames) {
    Asset* player = self_ptr();
    if (!player) {
        return;
    }

    if (Asset* interact_target = find_closest_tagged_asset(kInteractableTag, kInteractRadiusPx)) {
        custom_controller_api::dispatch_interact(player, interact_target);
        return;
    }

    if (Asset* carry_target = find_closest_tagged_asset(kCanCarryTag, kInteractRadiusPx)) {
        if (is_carrying_non_gun()) {
            drop_carried_asset(input, held_frames);
        }
        pickup_asset(*player, *carry_target);
        return;
    }

    drop_carried_asset(input, held_frames);
    ensure_hand_defaults();
}

bool vibble_controller::is_spider_egg_asset(const Asset* asset) const {
    if (!asset || !asset->info) {
        return false;
    }
    return normalized_tokens_equal(asset->info->name, kSpiderEggAssetName);
}

void vibble_controller::handle_sprint_dash_egg_disturbance(const Input& input) {
    runtime::context::GameRuntimeContext* runtime_ctx = mutable_runtime_game_context();
    if (!runtime_ctx) {
        return;
    }
    const bool sprinting = sprint_intent_active_;
    const bool dashing = isDashing;
    runtime_ctx->set_player_motion_disturbance(true, sprinting, dashing);
    drop_held_spider_egg_forced(input);
}

custom_controller_api::AttackProcessingConfig vibble_controller::attack_processing_config() const {
    custom_controller_api::AttackProcessingConfig config;
    config.max_knockback_distance = 120.0f;
    config.max_damage_for_knockback = 150;
    config.knockback_scale = 1.0f;
    config.hit_animation_id = "hit";
    config.death_animation_id = "die";
    config.hit_fallback_animation_id = animation_update::detail::kDefaultAnimation;
    config.death_fallback_tag = "break";
    return config;
}
