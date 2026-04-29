#include "animation/controllers/shared/random_orbit_3d_controller_behavior.hpp"

#include <algorithm>
#include <cmath>
#include <string>

#include "animation/animation_update.hpp"
#include "animation/controllers/shared/controller_game_context.hpp"
#include "animation/controllers/shared/custom_asset_controller.hpp"
#include "assets/asset/Asset.hpp"
#include "assets/asset/animation.hpp"
#include "assets/asset/asset_info.hpp"
#include "utils/input.hpp"

namespace animation_update::custom_controllers {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kTau = 6.28318530717958647692;

double wrap_angle(double angle) {
    while (angle > kPi) {
        angle -= kTau;
    }
    while (angle < -kPi) {
        angle += kTau;
    }
    return angle;
}

double blend_angle(double from, double to, double alpha) {
    const double clamped = std::clamp(alpha, 0.0, 1.0);
    const double delta = wrap_angle(to - from);
    return wrap_angle(from + delta * clamped);
}

double distance_3d(const axis::WorldPos& a, const axis::WorldPos& b) {
    const double dx = static_cast<double>(b.x - a.x);
    const double dy = static_cast<double>(b.y - a.y);
    const double dz = static_cast<double>(b.z - a.z);
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

double planar_distance(const axis::WorldPos& a, const axis::WorldPos& b) {
    const double dx = static_cast<double>(b.x - a.x);
    const double dz = static_cast<double>(b.z - a.z);
    return std::sqrt(dx * dx + dz * dz);
}

axis::WorldPos lerp_world(const axis::WorldPos& from, const axis::WorldPos& to, double t) {
    const double clamped = std::clamp(t, 0.0, 1.0);
    return axis::WorldPos{
        static_cast<int>(std::lround(from.x + static_cast<double>(to.x - from.x) * clamped)),
        static_cast<int>(std::lround(from.y + static_cast<double>(to.y - from.y) * clamped)),
        static_cast<int>(std::lround(from.z + static_cast<double>(to.z - from.z) * clamped))
    };
}

bool same_world_pos(const axis::WorldPos& lhs, const axis::WorldPos& rhs) {
    return lhs.x == rhs.x && lhs.y == rhs.y && lhs.z == rhs.z;
}

bool almost_equal(double lhs, double rhs) {
    return std::abs(lhs - rhs) <= 1e-6;
}

bool same_config(const RandomOrbit3DControllerBehaviorConfig& lhs,
                 const RandomOrbit3DControllerBehaviorConfig& rhs) {
    return lhs.visit_threshold_px == rhs.visit_threshold_px &&
           lhs.orbit_radius_px == rhs.orbit_radius_px &&
           lhs.orbit_vertical_amplitude_px == rhs.orbit_vertical_amplitude_px &&
           lhs.orbit_segment_checkpoints == rhs.orbit_segment_checkpoints &&
           lhs.orbit_enter_distance_px == rhs.orbit_enter_distance_px &&
           lhs.orbit_exit_distance_px == rhs.orbit_exit_distance_px &&
           lhs.approach_checkpoint_count == rhs.approach_checkpoint_count &&
           lhs.approach_min_wave_px == rhs.approach_min_wave_px &&
           lhs.approach_max_wave_px == rhs.approach_max_wave_px &&
           lhs.approach_vertical_wave_px == rhs.approach_vertical_wave_px &&
           almost_equal(lhs.orbit_angular_velocity_radians, rhs.orbit_angular_velocity_radians) &&
           almost_equal(lhs.retarget_blend_step, rhs.retarget_blend_step) &&
           almost_equal(lhs.non_aggressive_speed_multiplier, rhs.non_aggressive_speed_multiplier) &&
           almost_equal(lhs.aggressive_speed_multiplier, rhs.aggressive_speed_multiplier) &&
           lhs.debug_enabled == rhs.debug_enabled &&
           lhs.override_non_locked == rhs.override_non_locked;
}

} // namespace

RandomOrbit3DControllerBehavior::RandomOrbit3DControllerBehavior(
    CustomAssetController* controller,
    RandomOrbit3DControllerBehaviorConfig config)
    : controller_(controller),
      config_(config),
      rng_(std::random_device{}()) {
    angular_velocity_ = config_.orbit_angular_velocity_radians;
    if (std::abs(angular_velocity_) < 1e-4) {
        angular_velocity_ = 0.45;
    }
    if ((rng_() & 1u) == 0u) {
        angular_velocity_ = -angular_velocity_;
    }

    orbit_radius_goal_ =
        std::max(24.0, static_cast<double>(config_.orbit_radius_px) * orbit_radius_jitter_(rng_));
    orbit_radius_current_ = std::max(24.0, static_cast<double>(config_.orbit_radius_px));
    randomize_orbit_plane();

    if (!controller_) {
        return;
    }
    Asset* self = controller_->self_ptr();
    if (self) {
        self->needs_target = true;
    }
    if (self && self->anim_) {
        self->anim_->set_debug_enabled(config_.debug_enabled);
    }
}

void RandomOrbit3DControllerBehavior::set_config(RandomOrbit3DControllerBehaviorConfig config) {
    if (same_config(config_, config)) {
        return;
    }

    const bool debug_changed = config_.debug_enabled != config.debug_enabled;
    const bool radius_changed = config_.orbit_radius_px != config.orbit_radius_px;
    const bool velocity_changed = !almost_equal(config_.orbit_angular_velocity_radians,
                                                config.orbit_angular_velocity_radians);

    config_ = config;

    if (velocity_changed) {
        const double sign = (angular_velocity_ < 0.0) ? -1.0 : 1.0;
        angular_velocity_ = config_.orbit_angular_velocity_radians;
        if (std::abs(angular_velocity_) < 1e-4) {
            angular_velocity_ = 0.45;
        }
        angular_velocity_ = std::abs(angular_velocity_) * sign;
    }

    if (radius_changed) {
        orbit_radius_goal_ =
            std::max(24.0, static_cast<double>(config_.orbit_radius_px) * orbit_radius_jitter_(rng_));
        orbit_radius_current_ = std::max(24.0, orbit_radius_current_);
    }

    if (debug_changed && controller_) {
        Asset* self = controller_->self_ptr();
        if (self && self->anim_) {
            self->anim_->set_debug_enabled(config_.debug_enabled);
        }
    }
}

void RandomOrbit3DControllerBehavior::tick([[maybe_unused]] const Input& in) {
    if (!controller_) {
        return;
    }


    Asset* self = controller_->self_ptr();
    if (!self || !self->anim_) {
        return;
    }

    const auto& ctx = controller_->game_context();
    const axis::WorldPos self_pos{ self->world_x(), self->world_y(), self->world_z() };


    if (!ctx.fly_orbit_point.valid) {
        reset_for_missing_target(*self);
        return;
    }

    const axis::WorldPos target{
        ctx.fly_orbit_point.world_xz.x,
        ctx.fly_orbit_point.world_y,
        ctx.fly_orbit_point.world_xz.y
    };
    (void)ingest_target_snapshot(target,
                                 ctx.fly_orbit_target.target_version,
                                 ctx.fly_orbit_point.grid_resolution,
                                 ctx.fly_orbit_target_changed,
                                 self_pos);

    if (!has_target_) {
        reset_for_missing_target(*self);
        return;
    }

    if (phase_ != Phase::Orbit && should_enter_orbit(self_pos)) {
        phase_ = Phase::Orbit;
    } else if (phase_ == Phase::Orbit && should_exit_orbit(self_pos)) {
        phase_ = Phase::Approach;
    }

    const std::optional<int> resolution_override = checkpoint_resolution_override();
    bool moved = false;

    if (phase_ == Phase::Orbit) {
        const auto checkpoints = build_orbit_checkpoints(self_pos);
        if (!checkpoints.empty()) {
            moved = move_direct_towards(*self, self_pos, checkpoints.front(), resolution_override);
            const double speed = speed_multiplier();
            orbit_angle_ = wrap_angle(orbit_angle_ + (angular_velocity_ * speed));
            orbit_radius_current_ += (orbit_radius_goal_ - orbit_radius_current_) * 0.25;
            approach_wave_phase_ = wrap_angle(approach_wave_phase_ + 0.25 * speed);
        }
    } else {
        const bool soft_transition = phase_ == Phase::RetargetTransition;
        if (soft_transition) {
            retarget_alpha_ = std::min(1.0, retarget_alpha_ + std::max(0.05, config_.retarget_blend_step));
        }
        const axis::WorldPos goal = soft_transition ? blended_retarget_target() : target_;
        const auto checkpoints = build_approach_checkpoints(self_pos, goal, soft_transition);
        if (!checkpoints.empty()) {
            moved = move_direct_towards(*self, self_pos, checkpoints.front(), resolution_override);
            if (moved) {
                approach_wave_phase_ = wrap_angle(approach_wave_phase_ + 0.55 * speed_multiplier());
            }
            if (phase_ == Phase::RetargetTransition && retarget_alpha_ >= 1.0) {
                phase_ = Phase::Approach;
            }
        }
    }

    if (!moved) {
        apply_default_idle(*self);
    }
}

bool RandomOrbit3DControllerBehavior::ingest_target_snapshot(
    const axis::WorldPos& target,
    std::uint32_t target_version,
    int resolution_layer,
    bool changed_hint,
    const axis::WorldPos& self_pos) {
    const bool changed = changed_hint || !has_target_ || (target_version_ != target_version) ||
                         !same_world_pos(target_, target) ||
                         (checkpoint_resolution_layer_ != resolution_layer);
    if (!changed) {
        return false;
    }

    checkpoint_resolution_layer_ = resolution_layer;
    orbit_radius_goal_ =
        std::max(24.0, static_cast<double>(config_.orbit_radius_px) * orbit_radius_jitter_(rng_));

    if (!has_target_) {
        has_target_ = true;
        target_ = target;
        previous_target_ = target;
        target_version_ = target_version;
        phase_ = Phase::Approach;
        retarget_alpha_ = 1.0;
        randomize_orbit_plane();
        calibrate_orbit_phase(self_pos, target_);
        orbit_radius_current_ = std::max(24.0, planar_distance(self_pos, target_));
        return true;
    }

    previous_target_ = target_;
    target_ = target;
    target_version_ = target_version;
    phase_ = Phase::RetargetTransition;
    retarget_alpha_ = 0.0;
    randomize_orbit_plane();

    const double angle_to_new_target = std::atan2(
        static_cast<double>(self_pos.z - target_.z),
        static_cast<double>(self_pos.x - target_.x));
    orbit_angle_ = blend_angle(orbit_angle_, angle_to_new_target, 0.35);
    orbit_radius_current_ = std::max(24.0, orbit_radius_current_);
    return true;
}

void RandomOrbit3DControllerBehavior::reset_for_missing_target(Asset& self) {
    has_target_ = false;
    phase_ = Phase::Approach;
    retarget_alpha_ = 1.0;

    if (self.anim_ && !self.needs_target) {
        self.anim_->cancel_all_movement();
    }
    apply_default_idle(self);
}

void RandomOrbit3DControllerBehavior::apply_default_idle(Asset& self) const {
    if (!self.anim_ || !self.info) {
        return;
    }

    const std::string default_anim{ animation_update::detail::kDefaultAnimation };
    auto default_it = self.info->animations.find(default_anim);
    if (default_it != self.info->animations.end() && default_it->second.has_frames()) {
        if (self.current_animation != default_anim || self.current_frame == nullptr) {
            self.anim_->set_animation(default_anim);
        }
        return;
    }

    for (const auto& [anim_id, anim] : self.info->animations) {
        if (!anim.has_frames()) {
            continue;
        }
        if (self.current_animation != anim_id || self.current_frame == nullptr) {
            self.anim_->set_animation(anim_id);
        }
        return;
    }
}

std::optional<int> RandomOrbit3DControllerBehavior::checkpoint_resolution_override() const {
    if (checkpoint_resolution_layer_ < 0) {
        return std::nullopt;
    }
    return checkpoint_resolution_layer_;
}

bool RandomOrbit3DControllerBehavior::should_enter_orbit(const axis::WorldPos& self_pos) const {
    if (!has_target_) {
        return false;
    }

    const int enter_distance =
        std::max(config_.orbit_enter_distance_px, config_.orbit_radius_px + 32);
    return distance_3d(self_pos, target_) <= static_cast<double>(enter_distance);
}

bool RandomOrbit3DControllerBehavior::should_exit_orbit(const axis::WorldPos& self_pos) const {
    if (!has_target_) {
        return true;
    }

    const int exit_distance = std::max(
        config_.orbit_exit_distance_px,
        std::max(config_.orbit_enter_distance_px, config_.orbit_radius_px + 32) + 40);
    return distance_3d(self_pos, target_) > static_cast<double>(exit_distance);
}

double RandomOrbit3DControllerBehavior::speed_multiplier() const {
    if (!controller_) {
        return std::max(0.1, config_.non_aggressive_speed_multiplier);
    }
    const auto& ctx = controller_->game_context();
    const double configured = ctx.room_flies_aggressive()
        ? config_.aggressive_speed_multiplier
        : config_.non_aggressive_speed_multiplier;
    return std::max(0.1, configured);
}

int RandomOrbit3DControllerBehavior::movement_step_px() const {
    if (!controller_) {
        return 3;
    }
    return controller_->game_context().room_flies_aggressive() ? 20 : 10;
}

bool RandomOrbit3DControllerBehavior::move_direct_towards(
    Asset& self,
    const axis::WorldPos& from,
    const axis::WorldPos& target,
    std::optional<int> resolution_override) const {
    const double dx = static_cast<double>(target.x - from.x);
    const double dy = static_cast<double>(target.y - from.y);
    const double dz = static_cast<double>(target.z - from.z);
    const double dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (dist <= 0.5) {
        return false;
    }

    const double step = static_cast<double>(movement_step_px());
    const double t = std::min(1.0, step / dist);
    const int next_x = static_cast<int>(std::lround(static_cast<double>(from.x) + dx * t));
    const int next_y = static_cast<int>(std::lround(static_cast<double>(from.y) + dy * t));
    const int next_z = static_cast<int>(std::lround(static_cast<double>(from.z) + dz * t));

    if (next_x == from.x && next_y == from.y && next_z == from.z) {
        return false;
    }

    self.move_to_world_position(next_x, next_y, next_z, resolution_override);
    return true;
}

void RandomOrbit3DControllerBehavior::randomize_orbit_plane() {
    std::uniform_real_distribution<double> unit01(0.0, 1.0);
    const double phi = unit01(rng_) * kTau;
    const double cos_theta = 1.0 - (2.0 * unit01(rng_));
    const double sin_theta = std::sqrt(std::max(0.0, 1.0 - cos_theta * cos_theta));
    const std::array<double, 3> normal{
        std::cos(phi) * sin_theta,
        cos_theta,
        std::sin(phi) * sin_theta
    };

    std::array<double, 3> reference{0.0, 1.0, 0.0};
    if (std::abs(normal[1]) > 0.9) {
        reference = {1.0, 0.0, 0.0};
    }

    const std::array<double, 3> u_raw{
        reference[1] * normal[2] - reference[2] * normal[1],
        reference[2] * normal[0] - reference[0] * normal[2],
        reference[0] * normal[1] - reference[1] * normal[0]
    };
    const double u_len = std::sqrt(u_raw[0] * u_raw[0] + u_raw[1] * u_raw[1] + u_raw[2] * u_raw[2]);
    if (u_len <= 1e-6) {
        orbit_plane_u_ = {1.0, 0.0, 0.0};
        orbit_plane_v_ = {0.0, 0.0, 1.0};
        return;
    }
    orbit_plane_u_ = {u_raw[0] / u_len, u_raw[1] / u_len, u_raw[2] / u_len};

    const std::array<double, 3> v_raw{
        normal[1] * orbit_plane_u_[2] - normal[2] * orbit_plane_u_[1],
        normal[2] * orbit_plane_u_[0] - normal[0] * orbit_plane_u_[2],
        normal[0] * orbit_plane_u_[1] - normal[1] * orbit_plane_u_[0]
    };
    const double v_len = std::sqrt(v_raw[0] * v_raw[0] + v_raw[1] * v_raw[1] + v_raw[2] * v_raw[2]);
    if (v_len <= 1e-6) {
        orbit_plane_u_ = {1.0, 0.0, 0.0};
        orbit_plane_v_ = {0.0, 0.0, 1.0};
        return;
    }
    orbit_plane_v_ = {v_raw[0] / v_len, v_raw[1] / v_len, v_raw[2] / v_len};
}

axis::WorldPos RandomOrbit3DControllerBehavior::blended_retarget_target() const {
    return lerp_world(previous_target_, target_, retarget_alpha_);
}

void RandomOrbit3DControllerBehavior::calibrate_orbit_phase(const axis::WorldPos& self_pos,
                                                            const axis::WorldPos& center) {
    orbit_angle_ = std::atan2(static_cast<double>(self_pos.z - center.z),
                              static_cast<double>(self_pos.x - center.x));
    if (!std::isfinite(orbit_angle_)) {
        orbit_angle_ = 0.0;
    }
}

std::vector<axis::WorldPos> RandomOrbit3DControllerBehavior::build_approach_checkpoints(
    const axis::WorldPos& self_pos,
    const axis::WorldPos& target,
    bool soft_transition) const {
    std::vector<axis::WorldPos> checkpoints;

    const int count = std::clamp(config_.approach_checkpoint_count, 2, 8);
    const double dx = static_cast<double>(target.x - self_pos.x);
    const double dy = static_cast<double>(target.y - self_pos.y);
    const double dz = static_cast<double>(target.z - self_pos.z);
    const double planar_len = std::sqrt(dx * dx + dz * dz);
    const double dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (dist <= 1.0) {
        checkpoints.push_back(target);
        return checkpoints;
    }

    double perp_x = 1.0;
    double perp_z = 0.0;
    if (planar_len > 1e-6) {
        perp_x = -dz / planar_len;
        perp_z = dx / planar_len;
    }

    const double amplitude_min = static_cast<double>(std::max(0, config_.approach_min_wave_px));
    const double amplitude_max = static_cast<double>(std::max(config_.approach_min_wave_px,
                                                               config_.approach_max_wave_px));
    double amplitude = std::clamp(planar_len * 0.22, amplitude_min, amplitude_max);
    if (soft_transition) {
        amplitude *= 0.6;
    }
    const double vertical_amp = std::clamp(
        std::abs(dy) * 0.5,
        8.0,
        static_cast<double>(std::max(8, config_.approach_vertical_wave_px)));

    auto push_unique = [&](const axis::WorldPos& point) {
        if (!checkpoints.empty() && same_world_pos(checkpoints.back(), point)) {
            return;
        }
        checkpoints.push_back(point);
    };

    for (int idx = 1; idx < count; ++idx) {
        const double t = static_cast<double>(idx) / static_cast<double>(count);
        const axis::WorldPos base = lerp_world(self_pos, target, t);
        const double envelope = std::sin(t * kPi);
        const double phase = approach_wave_phase_ + (t * 1.25 * kTau);
        const double lateral = std::sin(phase) * amplitude * envelope;
        const double vertical = std::cos(phase * 0.5) * vertical_amp * envelope;

        const axis::WorldPos curved{
            static_cast<int>(std::lround(static_cast<double>(base.x) + perp_x * lateral)),
            static_cast<int>(std::lround(static_cast<double>(base.y) + vertical)),
            static_cast<int>(std::lround(static_cast<double>(base.z) + perp_z * lateral))
        };
        push_unique(curved);
    }

    push_unique(target);
    return checkpoints;
}



std::vector<axis::WorldPos> RandomOrbit3DControllerBehavior::build_orbit_checkpoints(
    const axis::WorldPos& self_pos) const {
    std::vector<axis::WorldPos> checkpoints;
    if (!has_target_) {
        return checkpoints;
    }

    const int steps = std::clamp(config_.orbit_segment_checkpoints, 2, 8);
    const double desired_radius = std::max(24.0, static_cast<double>(config_.orbit_radius_px));
    const double radius = std::max(
        24.0,
        (orbit_radius_current_ <= 1.0 ? planar_distance(self_pos, target_) : orbit_radius_current_) +
            (desired_radius - orbit_radius_current_) * 0.25);
    const double vertical_amp = std::max(0.0, static_cast<double>(config_.orbit_vertical_amplitude_px));
    const double speed = speed_multiplier();

    auto push_unique = [&](const axis::WorldPos& point) {
        if (!checkpoints.empty() && same_world_pos(checkpoints.back(), point)) {
            return;
        }
        checkpoints.push_back(point);
    };

    for (int idx = 1; idx <= steps; ++idx) {
        const double angle = orbit_angle_ + (angular_velocity_ * speed) * static_cast<double>(idx);
        const double cos_angle = std::cos(angle);
        const double sin_angle = std::sin(angle);
        const double vertical_wave = std::sin(angle + approach_wave_phase_ * 0.5) * vertical_amp;
        const double radial_u = cos_angle * radius;
        const double radial_v = sin_angle * radius;
        const axis::WorldPos point{
            target_.x + static_cast<int>(std::lround(orbit_plane_u_[0] * radial_u + orbit_plane_v_[0] * radial_v)),
            target_.y + static_cast<int>(std::lround(orbit_plane_u_[1] * radial_u + orbit_plane_v_[1] * radial_v + vertical_wave)),
            target_.z + static_cast<int>(std::lround(orbit_plane_u_[2] * radial_u + orbit_plane_v_[2] * radial_v))
        };
        push_unique(point);
    }

    return checkpoints;
}
}
// namespace animation_update::custom_controllers
