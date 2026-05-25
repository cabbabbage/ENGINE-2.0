#include "spider_controller.hpp"
#include "animation/animation_tag_utils.hpp"
#include "animation/animation_update.hpp"
#include "animation/attack_validation.hpp"
#include "animation/controllers/shared/attack_detection_helper.hpp"
#include "animation/controllers/shared/custom_controller_api.hpp"
#include "assets/asset/Asset.hpp"
#include "utils/frame_stats_recorder.hpp"
#include "utils/log.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <random>
#include <sstream>
#include <string>

namespace {

constexpr int kMaulRangePx = 320;
constexpr int kPounceRangePx = 1150;
constexpr int kPounceStopRangePx = 190;
constexpr int kPounceStepPx = 140;
constexpr int kMaulDamage = 34;
constexpr int kRetargetMs = 220;
constexpr int kMaulCooldownMs = 420;

long long distance_sq_xz(const Asset& a, const Asset& b) {
    const long long dx = static_cast<long long>(b.world_x()) - static_cast<long long>(a.world_x());
    const long long dz = static_cast<long long>(b.world_z()) - static_cast<long long>(a.world_z());
    return dx * dx + dz * dz;
}

bool current_animation_has_tag(const Asset& self, const char* tag) {
    if (!self.info) {
        return false;
    }
    const auto it = self.info->animations.find(self.current_animation);
    if (it == self.info->animations.end()) {
        return false;
    }
    return animation_update::tag_utils::has_normalized_tag(it->second.tags, tag);
}

std::string stable_asset_name_for_attack(const Asset& asset) {
    return asset.info ? asset.info->name : std::string{};
}

void trigger_maul_animation(Asset& self, const Asset& target) {
    if (!self.anim_) {
        return;
    }
    const char* facing_tag = target.world_x() < self.world_x() ? "left" : "right";
    if (self.anim_->set_animation_by_tags({"attack", facing_tag}, {})) {
        return;
    }
    (void)self.anim_->set_animation_by_tags({"attack"}, {});
}

void pounce_toward_player(Asset& self, const Asset& player, long long distance_sq) {
    if (!self.anim_ || current_animation_has_tag(self, "attack")) {
        return;
    }
    if (distance_sq <= static_cast<long long>(kPounceStopRangePx) * kPounceStopRangePx ||
        distance_sq > static_cast<long long>(kPounceRangePx) * kPounceRangePx) {
        return;
    }

    const double dx = static_cast<double>(player.world_x() - self.world_x());
    const double dz = static_cast<double>(player.world_z() - self.world_z());
    const double distance = std::sqrt(std::max(1.0, dx * dx + dz * dz));
    const int step = std::max(1, std::min(kPounceStepPx, static_cast<int>(std::lround(distance - kPounceStopRangePx))));
    SDL_Point delta{
        static_cast<int>(std::lround((dx / distance) * static_cast<double>(step))),
        static_cast<int>(std::lround((dz / distance) * static_cast<double>(step)))
    };
    if (delta.x == 0 && delta.y == 0) {
        delta.x = dx < 0.0 ? -1 : 1;
    }

    const std::string animation_id = delta.x < 0 ? "left" : "right";
    self.anim_->move(delta, animation_id);
}

} // namespace

spider_controller::spider_controller(Asset* self)
    : CustomAssetController(self) {
    Asset* owner = self_ptr();
    if (owner && owner->anim_) {
        owner->anim_->set_debug_enabled(false);
        owner->needs_target = true;
        owner->set_default_controller_animation_enforced(false);
    }
}

void spider_controller::on_update(const Input& in) {
    CustomAssetController::on_update(in);
    const auto& ctx = game_context();
    Asset* self = self_ptr();
    if (!self || !self->anim_ || !ctx.has_assets()) {
        return;
    }
    Asset* player = custom_controller_api::resolve_valid_player_target(ctx);

    if (!player) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const long long distance_sq = distance_sq_xz(*self, *player);
    const bool is_attacking = current_animation_has_tag(*self, "attack");
    if (!is_attacking && (self->needs_target || now >= next_retarget_time_)) {
        AnimationUpdate::AutoMoveCombatOverrides combat_overrides;
        combat_overrides.attacking_enabled = true;
        self->anim_->auto_move(player, 0, true, combat_overrides);
        next_retarget_time_ = now + std::chrono::milliseconds(kRetargetMs);
    }

    if (!is_attacking) {
        pounce_toward_player(*self, *player, distance_sq);
    }

    if (distance_sq <= static_cast<long long>(kMaulRangePx) * kMaulRangePx &&
        now >= next_maul_time_ &&
        !is_attacking) {
        trigger_maul_animation(*self, *player);
        next_maul_time_ = now + std::chrono::milliseconds(kMaulCooldownMs);

        auto& frame_stats = runtime_stats::FrameStatsRecorder::instance();
        frame_stats.set("combat.spider_maul_sent", true);
        frame_stats.set("combat.spider_maul_damage", kMaulDamage);
        frame_stats.set("combat.vibble_health_before_spider_maul", player->runtime_health);

        std::ostringstream oss;
        oss << "[Combat] Spider maul started target='" << stable_asset_name_for_attack(*player)
            << "' damage=" << kMaulDamage
            << " target_health_before=" << player->runtime_health
            << " distance_sq=" << distance_sq;
        vibble::log::info(oss.str());
    }

    // Ensure close-range attack candidates are still evaluated while
    // pathing updates are throttled by planner cooldowns.
    if (dispatch_weighted_attack_if_ready(*self, *player)) {
        on_after_attack();
    }
}

void spider_controller::on_process_pending_attacks(Asset& self) {
    CustomAssetController::on_process_pending_attacks(self);
}

bool spider_controller::dispatch_weighted_attack_if_ready(Asset& self, Asset& player) {
    const auto attack_opt = animation_update::AttackValidation::compute_attack_if_hit(self, player);
    if (!attack_opt.has_value()) {
        return false;
    }
    auto attack = *attack_opt;
    const std::string payload_id = attack.payload.payload_id.empty() ? attack.attack_payload_id : attack.payload.payload_id;
    const auto now = std::chrono::steady_clock::now();
    if (!payload_id.empty()) {
        const auto cooldown_it = payload_recharge_ready_at_.find(payload_id);
        if (cooldown_it != payload_recharge_ready_at_.end() && now < cooldown_it->second) {
            return false;
        }
    }

    const float weight = std::max(0.05f, attack.payload.recharge_random_weight);
    static thread_local std::mt19937 rng{std::random_device{}()};
    std::uniform_real_distribution<float> roll(0.0f, 1.0f);
    if (roll(rng) > std::min(1.0f, weight / 5.0f)) {
        return false;
    }

    player.send_attack(attack);
    if (!payload_id.empty()) {
        const float recharge_seconds = std::max(0.0f, attack.payload.recharge_seconds);
        payload_recharge_ready_at_[payload_id] =
            now + std::chrono::milliseconds(static_cast<int>(std::lround(recharge_seconds * 1000.0f)));
    }
    return true;
}
