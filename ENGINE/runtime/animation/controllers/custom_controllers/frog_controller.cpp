#include "frog_controller.hpp"

#include "assets/asset/Asset.hpp"

#include <algorithm>
#include <cmath>
#include <random>

namespace {

constexpr int kThreatRangePx = 96;
constexpr int kSafeDistancePx = 220;
constexpr int kRandomStepMinPx = 24;
constexpr int kRandomStepMaxPx = 72;
constexpr int kIdleFramesMin = 20;
constexpr int kIdleFramesMax = 100;

} // namespace

frog_controller::frog_controller(Asset* self)
    : custom_controller_api::CustomControllerBase(self),
      rng_(std::random_device{}()),
      idle_frames_remaining_(sample_idle_frames()) {
    Asset* owner = controller_self();
    if (owner && owner->anim_) {
        owner->anim_->set_debug_enabled(false);
        owner->needs_target = true;
    }
}

void frog_controller::on_update(const Input&) {
    Asset* self = controller_self();
    if (!self || !self->anim_) {
        return;
    }

    Asset* player = resolve_target_player();
    if (!player) {
        return;
    }

    const long long dx = static_cast<long long>(player->world_x()) - static_cast<long long>(self->world_x());
    const long long dy = static_cast<long long>(player->world_y()) - static_cast<long long>(self->world_y());
    const long long dz = static_cast<long long>(player->world_z()) - static_cast<long long>(self->world_z());
    const long long dist_sq = (dx * dx) + (dy * dy) + (dz * dz);
    const int threat_sq = kThreatRangePx * kThreatRangePx;
    const int safe_sq = kSafeDistancePx * kSafeDistancePx;

    custom_controller_api::MovementConfig flee_cfg{};
    flee_cfg.visit_threshold_px = 10;
    flee_cfg.override_non_locked = false;

    if (dist_sq <= static_cast<long long>(threat_sq)) {
        hop_away_from(*player);
        apply_attack_hit(*player);
        return;
    }

    if (dist_sq < static_cast<long long>(safe_sq) && flee_until_safe_) {
        hop_away_from(*player);
        apply_attack_hit(*player);
        return;
    }

    flee_until_safe_ = false;

    if (idle_frames_remaining_ > 0) {
        --idle_frames_remaining_;
        apply_attack_hit(*player);
        return;
    }

    random_wander_away_bias(*player);
    idle_frames_remaining_ = sample_idle_frames();

    apply_attack_hit(*player);
}

void frog_controller::on_process_pending_attacks(Asset& self) {
    custom_controller_api::CustomControllerBase::on_process_pending_attacks(self);
}

int frog_controller::sample_idle_frames() {
    std::uniform_int_distribution<int> distribution(kIdleFramesMin, kIdleFramesMax);
    return distribution(rng_);
}

void frog_controller::hop_away_from(const Asset& player) {
    Asset* self = controller_self();
    if (!self) {
        return;
    }

    flee_until_safe_ = true;
    custom_controller_api::MovementConfig move_cfg{};
    move_cfg.visit_threshold_px = 12;
    move_cfg.override_non_locked = false;

    (void)move_away(axis::WorldPos{player.world_x(), player.world_y(), player.world_z()},
                    kRandomStepMaxPx,
                    move_cfg);
}

void frog_controller::random_wander_away_bias(const Asset& player) {
    Asset* self = controller_self();
    if (!self) {
        return;
    }

    std::uniform_real_distribution<double> noise(-0.55, 0.55);
    std::uniform_int_distribution<int> step_dist(kRandomStepMinPx, kRandomStepMaxPx);

    const int away_x = self->world_x() - player.world_x();
    const int away_z = self->world_z() - player.world_z();
    double vx = static_cast<double>(away_x);
    double vz = static_cast<double>(away_z);
    const double base_len = std::sqrt(vx * vx + vz * vz);
    if (base_len < 0.001) {
        std::uniform_real_distribution<double> dir(-1.0, 1.0);
        vx = dir(rng_);
        vz = dir(rng_);
    }

    const double len = std::sqrt(std::max(0.0001, vx * vx + vz * vz));
    vx /= len;
    vz /= len;

    vx += noise(rng_);
    vz += noise(rng_);

    const double noisy_len = std::sqrt(std::max(0.0001, vx * vx + vz * vz));
    vx /= noisy_len;
    vz /= noisy_len;

    const int step = step_dist(rng_);
    const axis::WorldPos target{
        self->world_x() + static_cast<int>(std::lround(vx * step)),
        self->world_y(),
        self->world_z() + static_cast<int>(std::lround(vz * step))};

    custom_controller_api::MovementConfig move_cfg{};
    move_cfg.visit_threshold_px = 8;
    move_cfg.override_non_locked = false;
    (void)move_toward(target, step, move_cfg);
}
