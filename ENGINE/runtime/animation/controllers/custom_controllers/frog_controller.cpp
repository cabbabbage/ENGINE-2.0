#include "frog_controller.hpp"
#include "animation/controllers/shared/custom_controller_api.hpp"
#include "animation/animation_update.hpp"

#include "animation/controllers/shared/controller_path_utils.hpp"
#include "animation/controllers/shared/controller_visit_threshold.hpp"
#include "assets/asset/Asset.hpp"
#include "utils/range_util.hpp"

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

int distance_sq_xz(const Asset& a, const Asset& b) {
    const int dx = b.world_x() - a.world_x();
    const int dz = b.world_z() - a.world_z();
    return dx * dx + dz * dz;
}

} // namespace

frog_controller::frog_controller(Asset* self)
    : CustomAssetController(self),
      rng_(std::random_device{}()),
      idle_frames_remaining_(sample_idle_frames()) {
    Asset* owner = self_ptr();
    if (owner && owner->anim_) {
        owner->anim_->set_debug_enabled(false);
        owner->needs_target = true;
    }
}

void frog_controller::on_update(const Input&) {
    const auto& ctx = game_context();
    Asset* self = self_ptr();
    if (!self || !self->anim_ || !ctx.has_assets()) {
        return;
    }

    Asset* player = custom_controller_api::resolve_valid_player_target(ctx);
    if (!player) {
        return;
    }

    const int dist_sq = distance_sq_xz(*self, *player);
    const int threat_sq = kThreatRangePx * kThreatRangePx;
    const int safe_sq = kSafeDistancePx * kSafeDistancePx;

    if (dist_sq <= threat_sq) {
        hop_away_from(*player);
        custom_controller_api::dispatch_contact_attack(ctx);
        return;
    }

    if (dist_sq < safe_sq && flee_until_safe_) {
        hop_away_from(*player);
        custom_controller_api::dispatch_contact_attack(ctx);
        return;
    }

    flee_until_safe_ = false;

    if (idle_frames_remaining_ > 0) {
        --idle_frames_remaining_;
        custom_controller_api::dispatch_contact_attack(ctx);
        return;
    }

    random_wander_away_bias(*player);
    idle_frames_remaining_ = sample_idle_frames();

    custom_controller_api::dispatch_contact_attack(ctx);
}

void frog_controller::on_process_pending_attacks(Asset& self) {
    CustomAssetController::on_process_pending_attacks(self);
}

int frog_controller::sample_idle_frames() {
    std::uniform_int_distribution<int> distribution(kIdleFramesMin, kIdleFramesMax);
    return distribution(rng_);
}

void frog_controller::hop_away_from(const Asset& player) {
    Asset* self = self_ptr();
    if (!self || !self->anim_) {
        return;
    }

    flee_until_safe_ = true;

    const auto path = controller_paths::flee_path(self, &player);
    if (!path.empty()) {
        const int visit_threshold = controller_utils::controller_visit_threshold(self, path);
        self->anim_->auto_move(path, visit_threshold);
        return;
    }

    const int dx = self->world_x() - player.world_x();
    const int dz = self->world_z() - player.world_z();
    const double len = std::sqrt(std::max(1.0, static_cast<double>(dx * dx + dz * dz)));
    const int step = kRandomStepMaxPx;
    SDL_Point delta{
        static_cast<int>(std::lround((static_cast<double>(dx) / len) * step)),
        static_cast<int>(std::lround((static_cast<double>(dz) / len) * step))
    };
    if (delta.x == 0 && delta.y == 0) {
        delta.x = (dx < 0) ? -1 : 1;
    }
    self->anim_->move(delta, delta.x < 0 ? "left" : "right");
}

void frog_controller::random_wander_away_bias(const Asset& player) {
    Asset* self = self_ptr();
    if (!self || !self->anim_) {
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
    SDL_Point delta{
        static_cast<int>(std::lround(vx * step)),
        static_cast<int>(std::lround(vz * step))
    };
    if (delta.x == 0 && delta.y == 0) {
        delta.x = 1;
    }

    self->anim_->move(delta, delta.x < 0 ? "left" : "right");
}
