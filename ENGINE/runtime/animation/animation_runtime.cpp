#include "animation_runtime.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>
#include <unordered_map>

#include "assets/asset/Asset.hpp"
#include "utils/log.hpp"
#include "assets/asset/animation.hpp"
#include "assets/asset/animation_frame.hpp"
#include "assets/asset/asset_info.hpp"
#include "assets/asset/asset_types.hpp"
#include "core/AssetsManager.hpp"
#include "core/dev_mode_animation_policy.hpp"
#include "movement_plan_executor.hpp"
#include "path_sanitizer.hpp"
#include "get_best_path.hpp"
#include "utils/area.hpp"
#include "utils/grid.hpp"
#include "rendering/render/warped_screen_grid.hpp"
#include <iostream>
#include "animation_update.hpp"
#include "animation/animation_tag_utils.hpp"
#include "animation/attack_validation.hpp"
#include "utils/frame_stats_recorder.hpp"
#include "utils/transform_smoothing.hpp"
#include "unstick_utils.hpp"

namespace {
constexpr int kMaxBlockedMoveFallbackProbes = 12;
constexpr int kLocalImpassableQueryRadiusPx = 256;
constexpr int kSegmentImpassableMinRadiusPx = 64;
constexpr int kUnstickMinQueryRadiusPx = 96;
constexpr int kUnstickRadiusPaddingPx = 32;

SDL_Point point_on_delta(SDL_Point from, SDL_Point delta, int step, int steps) {
    if (steps <= 0) {
        return from;
    }
    const double t = static_cast<double>(step) / static_cast<double>(steps);
    return SDL_Point{
        static_cast<int>(std::round(static_cast<double>(from.x) + static_cast<double>(delta.x) * t)),
        static_cast<int>(std::round(static_cast<double>(from.y) + static_cast<double>(delta.y) * t))
    };
}

template <typename Fn>
bool visit_impassable_neighbors(const Asset& asset, Fn&& fn) {
    const Assets* assets = asset.get_assets();
    if (!assets) {
        return false;
    }

    std::vector<const Assets::FrameCollisionEntry*> entries;
    const int search_radius = std::min(kLocalImpassableQueryRadiusPx,
                                       assets->max_impassable_query_radius());
    assets->query_impassable_entries(asset, search_radius, entries);

    for (const Assets::FrameCollisionEntry* entry : entries) {
        if (!entry) {
            continue;
        }
        if (fn(entry, entry->asset, entry->area, entry->bottom_middle)) {
            return true;
        }
    }

    return false;
}

std::string resolve_animation(const Asset& asset, const std::string& requested) {
    if (!asset.info) {
        return animation_update::detail::kDefaultAnimation;
    }

    if (!requested.empty()) {
        auto it = asset.info->animations.find(requested);
        if (it != asset.info->animations.end()) {
            return it->first;
        }
    }

    return animation_update::detail::kDefaultAnimation;
}

bool same_point(SDL_Point lhs, SDL_Point rhs) {
    return lhs.x == rhs.x && lhs.y == rhs.y;
}

std::uint64_t fnv1a_64(std::string_view text) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (unsigned char ch : text) {
        hash ^= static_cast<std::uint64_t>(ch);
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::uint64_t mix64(std::uint64_t value) {
    value ^= value >> 30;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27;
    value *= 0x94d049bb133111ebULL;
    value ^= value >> 31;
    return value;
}

std::uint64_t path_variance_seed_for_retry(const Asset& self,
                                           std::size_t checkpoint_index,
                                           std::uint32_t retry_counter,
                                           std::uint64_t salt = 0) {
    const std::string stable_id = animation_update::detail::stable_asset_id(self);
    std::uint64_t seed = fnv1a_64(stable_id);
    seed ^= mix64((static_cast<std::uint64_t>(checkpoint_index) + 1) * 0x9e3779b97f4a7c15ULL);
    seed ^= mix64((static_cast<std::uint64_t>(retry_counter) + 1) * 0xd6e8feb86659fd93ULL);
    seed ^= mix64(salt + 0xa0761d6478bd642fULL);
    return mix64(seed);
}

void reorder_directions_for_retry(std::vector<SDL_Point>& directions, std::uint64_t seed) {
    if (directions.size() < 2) {
        return;
    }
    const std::size_t offset = static_cast<std::size_t>(seed % directions.size());
    std::rotate(directions.begin(), directions.begin() + offset, directions.end());
    if (((seed >> 8) & 1ULL) != 0ULL) {
        std::reverse(directions.begin(), directions.end());
    }
}

template <typename Fn>
bool visit_impassable_neighbors_for_segment(const Asset& asset,
                                            SDL_Point from,
                                            SDL_Point to,
                                            Fn&& fn) {
    const Assets* assets = asset.get_assets();
    if (!assets) {
        return false;
    }

    std::vector<const Assets::FrameCollisionEntry*> entries;
    const SDL_Point self_center = asset.world_xz_point();
    const std::int64_t dx_from = static_cast<std::int64_t>(from.x) - static_cast<std::int64_t>(self_center.x);
    const std::int64_t dy_from = static_cast<std::int64_t>(from.y) - static_cast<std::int64_t>(self_center.y);
    const std::int64_t dx_to = static_cast<std::int64_t>(to.x) - static_cast<std::int64_t>(self_center.x);
    const std::int64_t dy_to = static_cast<std::int64_t>(to.y) - static_cast<std::int64_t>(self_center.y);
    const std::int64_t max_dist_sq =
        std::max(dx_from * dx_from + dy_from * dy_from, dx_to * dx_to + dy_to * dy_to);
    const int segment_coverage_radius = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(max_dist_sq))));
    const int capped_max_radius = assets->max_impassable_query_radius();
    const int query_radius = std::min(
        std::max(kSegmentImpassableMinRadiusPx, segment_coverage_radius + kUnstickRadiusPaddingPx),
        capped_max_radius);

    assets->query_impassable_entries(asset, query_radius, entries);

    for (const Assets::FrameCollisionEntry* entry : entries) {
        if (!entry) {
            continue;
        }
        if (fn(entry, entry->asset, entry->area, entry->bottom_middle)) {
            return true;
        }
    }
    return false;
}

int resolve_effective_grid_resolution(const Asset* self,
                                      const vibble::grid::Grid& grid_service,
                                      std::optional<int> override_resolution) {
    if (override_resolution.has_value()) {
        return vibble::grid::clamp_resolution(*override_resolution);
    }
    if (self) {
        return vibble::grid::clamp_resolution(self->grid_resolution);
    }
    return grid_service.default_resolution();
}

enum class HorizontalFacingIntent {
    Unknown = 0,
    Left,
    Right,
};

HorizontalFacingIntent facing_intent_for_delta_x(int delta_x, int deadzone_px) {
    const int clamped_deadzone = std::max(0, deadzone_px);
    if (delta_x > clamped_deadzone) {
        return HorizontalFacingIntent::Right;
    }
    if (delta_x < -clamped_deadzone) {
        return HorizontalFacingIntent::Left;
    }
    return HorizontalFacingIntent::Unknown;
}

std::string lower_ascii_copy(const std::string& value) {
    std::string out = value;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return out;
}

bool frame_has_damage_attack_box(const AnimationFrame& frame) {
    for (const auto& attack_box : frame.attack_boxes.boxes) {
        if (!attack_box.enabled) {
            continue;
        }
        const int damage_amount = std::max(attack_box.damage_amount, attack_box.payload.damage_amount);
        if (damage_amount > 0) {
            return true;
        }
    }
    return false;
}

bool animation_has_damage_attack_boxes(const Animation& animation) {
    const std::size_t path_count = animation.movement_path_count();
    for (std::size_t path_index = 0; path_index < path_count; ++path_index) {
        const auto& path = animation.movement_path(path_index);
        for (const auto& frame : path) {
            if (frame_has_damage_attack_box(frame)) {
                return true;
            }
        }
    }
    return false;
}

bool animation_is_attack_candidate(const Animation& animation) {
    return animation_update::tag_utils::has_normalized_tag(animation.tags, "attack") ||
           animation_has_damage_attack_boxes(animation);
}

HorizontalFacingIntent attack_facing_intent(const std::vector<std::string>& tags,
                                            const std::string& animation_id) {
    const bool has_left_tag = animation_update::tag_utils::has_normalized_tag(tags, "left");
    const bool has_right_tag = animation_update::tag_utils::has_normalized_tag(tags, "right");
    if (has_left_tag != has_right_tag) {
        return has_right_tag ? HorizontalFacingIntent::Right : HorizontalFacingIntent::Left;
    }

    const std::string lowered_id = lower_ascii_copy(animation_id);
    const bool id_has_left = lowered_id.find("left") != std::string::npos;
    const bool id_has_right = lowered_id.find("right") != std::string::npos;
    if (id_has_left != id_has_right) {
        return id_has_right ? HorizontalFacingIntent::Right : HorizontalFacingIntent::Left;
    }

    return HorizontalFacingIntent::Unknown;
}

int attack_facing_match_score_impl(const std::vector<std::string>& animation_tags,
                                   const std::string& animation_id,
                                   int target_delta_x,
                                   int deadzone_px) {
    const HorizontalFacingIntent target_intent = facing_intent_for_delta_x(target_delta_x, deadzone_px);
    const HorizontalFacingIntent attack_intent = attack_facing_intent(animation_tags, animation_id);
    if (target_intent == HorizontalFacingIntent::Unknown || attack_intent == HorizontalFacingIntent::Unknown) {
        return 0;
    }
    return target_intent == attack_intent ? 1 : -1;
}

} // namespace



struct MovementController {
    static bool consume_replan_attempt_budget(AnimationRuntime& runtime, std::uint32_t frame_id) {
        if (runtime.movement_state_.replan_budget_frame_id != frame_id) {
            runtime.movement_state_.replan_budget_frame_id = frame_id;
            runtime.movement_state_.replan_attempts_this_frame = 0;
        }
        if (runtime.movement_state_.replan_attempts_this_frame >= AnimationRuntime::kMaxReplanAttemptsPerFrame) {
            return false;
        }
        ++runtime.movement_state_.replan_attempts_this_frame;
        ++runtime.movement_state_.replan_attempt_counter;
        return true;
    }
};

struct PlaybackController {
    static void clear_reverse(AnimationRuntime& runtime) {
        runtime.playback_state_.reverse_mode = AnimationRuntime::ReversePlaybackMode::None;
        runtime.playback_state_.reverse_animation_id.clear();
    }
};

struct CombatController {
    static void clear_commitment(AnimationRuntime& runtime) {
        runtime.combat_state_.committed_attack_target_asset_id = std::nullopt;
        runtime.combat_state_.committed_attack_animation_id.clear();
        runtime.combat_state_.committed_attack_path_index = 0;
        runtime.combat_state_.committed_attack_last_dispatched_frame_index = -1;
        runtime.combat_state_.committed_attack_last_payload_id.clear();
        runtime.combat_state_.attack_recovery_pending = false;
        runtime.combat_state_.attack_recovery_animation_id.clear();
    }
};
namespace animation_runtime::test_hooks {

int attack_facing_match_score(const std::vector<std::string>& animation_tags,
                              const std::string& animation_id,
                              int target_delta_x,
                              int deadzone_px) {
    return attack_facing_match_score_impl(animation_tags, animation_id, target_delta_x, deadzone_px);
}

void force_committed_attack_target(AnimationRuntime& runtime, std::string target_asset_id) {
    runtime.combat_state_.committed_attack_target_asset_id = std::move(target_asset_id);
    runtime.combat_state_.committed_attack_animation_id =
        runtime.self_ ? runtime.self_->current_animation : std::string{};
    runtime.combat_state_.committed_attack_path_index =
        runtime.self_ ? runtime.path_index_for(runtime.self_->current_animation) : 0;
    runtime.combat_state_.committed_attack_last_dispatched_frame_index = -1;
    runtime.combat_state_.committed_attack_last_payload_id.clear();
}

}

AnimationRuntime::AnimationRuntime(Asset* self, Assets* assets)
    : self_(self), assets_owner_(assets), grid_service_(&vibble::grid::global_grid()) {}

bool AnimationRuntime::consume_replan_attempt_budget() {
    const Assets* assets = assets_owner_ ? assets_owner_ : (self_ ? self_->get_assets() : nullptr);
    const std::uint32_t frame_id = assets ? assets->frame_id() : 0;
    return MovementController::consume_replan_attempt_budget(*this, frame_id);
}

std::uint32_t AnimationRuntime::resolve_frame_id_for_cooldown() {
    if (assets_owner_) {
        return assets_owner_->frame_id();
    }
    ++local_runtime_frame_id_;
    if (local_runtime_frame_id_ == 0) {
        ++local_runtime_frame_id_;
    }
    return local_runtime_frame_id_;
}

bool AnimationRuntime::attacking_enabled_for_self() const {
    if (!self_ || !self_->info) {
        return false;
    }
    for (const auto& [animation_id, animation] : self_->info->animations) {
        (void)animation_id;
        if (animation_is_attack_candidate(animation)) {
            return true;
        }
    }
    return false;
}

bool AnimationRuntime::attacking_enabled_for_active_plan() const {
    if (!attacking_enabled_for_self()) {
        return false;
    }
    if (!planner_iface_) {
        return true;
    }
    return planner_iface_->auto_move_attacking_enabled_;
}

Asset* AnimationRuntime::resolve_asset_by_stable_id(const std::string& stable_id) const {
    if (stable_id.empty() || !assets_owner_) {
        return nullptr;
    }
    for (Asset* candidate : assets_owner_->getActive()) {
        if (!candidate || candidate == self_ || candidate->dead || !candidate->active) {
            continue;
        }
        if (animation_update::detail::stable_asset_id(*candidate) == stable_id) {
            return candidate;
        }
    }
    Asset* player = assets_owner_->game_context().player();
    if (!player) {
        player = assets_owner_->player;
    }
    if (player && player != self_ && player->active && !player->dead &&
        animation_update::detail::stable_asset_id(*player) == stable_id) {
        return player;
    }
    return nullptr;
}

std::vector<std::string> AnimationRuntime::attack_animation_candidates() const {
    std::vector<std::string> out;
    if (!self_ || !self_->info) {
        return out;
    }
    for (const auto& [animation_id, animation] : self_->info->animations) {
        if (!animation.has_frames()) {
            continue;
        }
        if (animation_is_attack_candidate(animation)) {
            out.push_back(animation_id);
        }
    }
    return out;
}

std::vector<Asset*> AnimationRuntime::attack_candidate_targets() const {
    std::vector<Asset*> out;
    if (!self_ || !assets_owner_) {
        return out;
    }

    const bool enemy_asset =
        self_->info &&
        asset_types::canonicalize(self_->info->type) == asset_types::enemy;

    Asset* player = assets_owner_->game_context().player();
    if (!player) {
        player = assets_owner_->player;
    }
    const bool player_valid =
        player && player != self_ && player->active && !player->dead && player->isHitboxEnabled();

    if (enemy_asset) {
        if (player_valid) {
            out.push_back(player);
        } else if (debug_enabled_) {
            const std::string self_name =
                (self_->info && !self_->info->name.empty()) ? self_->info->name : std::string{"<unknown>"};
            vibble::log::info("[AICombat] Rejecting attack target acquisition for enemy '" + self_name +
                              "': player unavailable or not hittable");
        }
        return out;
    }

    if (planner_iface_ && planner_iface_->pending_engagement_target_asset_id_.has_value()) {
        if (Asset* engagement_target =
                resolve_asset_by_stable_id(*planner_iface_->pending_engagement_target_asset_id_)) {
            if (engagement_target->isHitboxEnabled()) {
                out.push_back(engagement_target);
            }
        }
    }

    if (planner_iface_) {
        const std::optional<std::string> engagement_target_id =
            planner_iface_->active_plan_mode_ == AnimationUpdate::ActivePlanMode::Plan2D
                ? planner_iface_->plan_.engagement_target_asset_id
                : planner_iface_->active_plan_mode_ == AnimationUpdate::ActivePlanMode::Plan3D
                      ? planner_iface_->plan3d_.engagement_target_asset_id
                      : std::nullopt;
        if (engagement_target_id.has_value()) {
            if (Asset* engagement_target = resolve_asset_by_stable_id(*engagement_target_id)) {
                if (engagement_target->isHitboxEnabled() &&
                    std::find(out.begin(), out.end(), engagement_target) == out.end()) {
                    out.push_back(engagement_target);
                }
            }
        }
    }

    if (player_valid) {
        out.push_back(player);
    }

    for (Asset* candidate : assets_owner_->getActive()) {
        if (!candidate || candidate == self_ || candidate->dead || !candidate->active || !candidate->isHitboxEnabled()) {
            continue;
        }
        if (std::find(out.begin(), out.end(), candidate) != out.end()) {
            continue;
        }
        out.push_back(candidate);
    }
    return out;
}

bool AnimationRuntime::maybe_trigger_attack_on_cycle_boundary() {
    const bool committed_cycle_boundary = current_animation_is_attack() && combat_state_.committed_attack_target_asset_id.has_value();
    if (committed_cycle_boundary) {
        clear_attack_commitment();
    }
    if (attack_recovery_sequence_active()) {
        return false;
    }
    if (!self_ || !self_->info || !attacking_enabled_for_active_plan()) {
        if (committed_cycle_boundary && current_animation_is_attack()) {
            switch_to(animation_update::detail::kDefaultAnimation,
                      path_index_for(animation_update::detail::kDefaultAnimation),
                      TransitionLockPolicy::RespectCurrentLock);
        }
        return false;
    }
    const std::uint32_t frame_id = resolve_frame_id_for_cooldown();
    if (!committed_cycle_boundary &&
        combat_state_.next_attack_cycle_eval_frame != 0 &&
        frame_id < combat_state_.next_attack_cycle_eval_frame) {
        return false;
    }

    const auto targets = attack_candidate_targets();
    if (targets.empty()) {
        if (debug_enabled_) {
            const std::string self_name =
                (self_->info && !self_->info->name.empty()) ? self_->info->name : std::string{"<unknown>"};
            vibble::log::info("[AICombat] No valid attack targets for '" + self_name + "'");
        }
        if (committed_cycle_boundary && current_animation_is_attack()) {
            switch_to(animation_update::detail::kDefaultAnimation,
                      path_index_for(animation_update::detail::kDefaultAnimation),
                      TransitionLockPolicy::RespectCurrentLock);
        }
        return false;
    }
    const auto attack_candidates = attack_animation_candidates();
    if (attack_candidates.empty()) {
        if (debug_enabled_) {
            const std::string self_name =
                (self_->info && !self_->info->name.empty()) ? self_->info->name : std::string{"<unknown>"};
            vibble::log::info("[AICombat] No valid attack animations for '" + self_name + "'");
        }
        if (committed_cycle_boundary && current_animation_is_attack()) {
            switch_to(animation_update::detail::kDefaultAnimation,
                      path_index_for(animation_update::detail::kDefaultAnimation),
                      TransitionLockPolicy::RespectCurrentLock);
        }
        return false;
    }

    struct RankedChoice {
        animation_update::AttackValidation::AttackWindowScore score =
            animation_update::AttackValidation::AttackWindowScore::Miss;
        int facing_score = -2;
        std::string animation_id{};
        std::size_t path_index = 0;
        std::string target_asset_id{};
    } best{};
    bool has_best = false;

    auto better_choice = [](const RankedChoice& candidate, const RankedChoice& current_best) {
        const int candidate_score = static_cast<int>(candidate.score);
        const int best_score = static_cast<int>(current_best.score);
        if (candidate_score != best_score) {
            return candidate_score > best_score;
        }
        if (candidate.facing_score != current_best.facing_score) {
            return candidate.facing_score > current_best.facing_score;
        }
        if (candidate.target_asset_id != current_best.target_asset_id) {
            return candidate.target_asset_id < current_best.target_asset_id;
        }
        if (candidate.path_index != current_best.path_index) {
            return candidate.path_index < current_best.path_index;
        }
        return candidate.animation_id < current_best.animation_id;
    };

    constexpr int kAttackFacingDeadzonePx = 6;

    for (Asset* target : targets) {
        const std::string target_id = animation_update::detail::stable_asset_id(*target);
        if (target_id.empty()) {
            continue;
        }
        const auto ranked =
            animation_update::AttackValidation::rank_attack_candidates(
                *self_,
                *target,
                attack_candidates,
                8,
                true);
        if (!ranked.has_value()) {
            if (debug_enabled_) {
                const std::string target_name =
                    (target->info && !target->info->name.empty()) ? target->info->name : std::string{"<unknown>"};
                vibble::log::info("[AICombat] Rejecting target '" + target_name +
                                  "': no hittable attack window");
            }
            continue;
        }

        const auto& candidate_ranked = *ranked;
        const int target_delta_x = target->world_x() - self_->world_x();
        int facing_score = 0;
        const auto attack_it = self_->info->animations.find(candidate_ranked.animation_id);
        if (attack_it != self_->info->animations.end()) {
            facing_score =
                attack_facing_match_score_impl(attack_it->second.tags,
                                               candidate_ranked.animation_id,
                                               target_delta_x,
                                               kAttackFacingDeadzonePx);
        }

        RankedChoice candidate{};
        candidate.score = candidate_ranked.evaluation.score;
        candidate.facing_score = facing_score;
        candidate.animation_id = candidate_ranked.animation_id;
        candidate.path_index = candidate_ranked.path_index;
        candidate.target_asset_id = target_id;
        if (!has_best || better_choice(candidate, best)) {
            best = std::move(candidate);
            has_best = true;
        }
    }

    if (!has_best || best.animation_id.empty() || best.target_asset_id.empty()) {
        if (committed_cycle_boundary && current_animation_is_attack()) {
            switch_to(animation_update::detail::kDefaultAnimation,
                      path_index_for(animation_update::detail::kDefaultAnimation),
                      TransitionLockPolicy::RespectCurrentLock);
        }
        return false;
    }
    combat_state_.committed_attack_target_asset_id = best.target_asset_id;
    combat_state_.committed_attack_animation_id = best.animation_id;
    combat_state_.committed_attack_path_index = best.path_index;
    combat_state_.committed_attack_last_dispatched_frame_index = -1;
    combat_state_.committed_attack_last_payload_id.clear();
    if (debug_enabled_) {
        const std::string self_name =
            (self_->info && !self_->info->name.empty()) ? self_->info->name : std::string{"<unknown>"};
        vibble::log::info("[AICombat] Attack committed for '" + self_name +
                          "': animation='" + best.animation_id +
                          "' target_id='" + best.target_asset_id +
                          "' score=" + std::to_string(static_cast<int>(best.score)) +
                          " facing_score=" + std::to_string(best.facing_score));
    }
    if (!switch_to(best.animation_id, best.path_index, TransitionLockPolicy::RespectCurrentLock)) {
        clear_attack_commitment();
        return false;
    }
    combat_state_.next_attack_cycle_eval_frame = frame_id + kAttackCycleDebounceFrames;
    return true;
}

bool AnimationRuntime::current_animation_is_attack() const {
    if (!self_ || !self_->info) {
        return false;
    }
    const auto it = self_->info->animations.find(self_->current_animation);
    if (it == self_->info->animations.end()) {
        return false;
    }
    return animation_is_attack_candidate(it->second);
}

bool AnimationRuntime::auto_attack_commitment_active() const {
    if (attack_recovery_sequence_active()) {
        return true;
    }
    if (!self_ || !self_->info || !current_animation_is_attack()) {
        return false;
    }
    if (combat_state_.committed_attack_target_asset_id.has_value()) {
        return true;
    }
    return !combat_state_.committed_attack_animation_id.empty() &&
           combat_state_.committed_attack_animation_id == self_->current_animation;
}

void AnimationRuntime::clear_attack_commitment() {
    CombatController::clear_commitment(*this);
}

bool AnimationRuntime::attack_recovery_sequence_active() const {
    return combat_state_.attack_recovery_pending && !combat_state_.attack_recovery_animation_id.empty();
}

void AnimationRuntime::dispatch_active_attack_payload() {
    if (attack_recovery_sequence_active()) {
        return;
    }
    if (!self_ || !self_->info || !attacking_enabled_for_active_plan()) {
        clear_attack_commitment();
        return;
    }
    if (!current_animation_is_attack()) {
        return;
    }
    if (!combat_state_.committed_attack_target_asset_id.has_value()) {
        if (Asset* fallback = resolve_fallback_attack_target()) {
            combat_state_.committed_attack_target_asset_id =
                animation_update::detail::stable_asset_id(*fallback);
            runtime_stats::FrameStatsRecorder::instance().set(
                "combat.attack_payload_fallback_target_committed", true);
        } else {
            return;
        }
    }
    if (path_index_for(self_->current_animation) != combat_state_.committed_attack_path_index) {
        clear_attack_commitment();
        return;
    }

    Asset* target = resolve_asset_by_stable_id(*combat_state_.committed_attack_target_asset_id);
    if (!target || !target->active || target->dead || !target->isHitboxEnabled()) {
        if (debug_enabled_) {
            vibble::log::info("[AICombat] Clearing committed attack target: target invalid before dispatch");
        }
        clear_attack_commitment();
        return;
    }

    const auto attack_opt = animation_update::AttackValidation::compute_attack_if_hit(*self_, *target);
    if (!attack_opt.has_value()) {
        if (debug_enabled_) {
            const std::string target_name =
                (target->info && !target->info->name.empty()) ? target->info->name : std::string{"<unknown>"};
            vibble::log::info("[AICombat] Attack frame not hittable against '" + target_name + "'");
        }
        return;
    }

    const animation_update::Attack& attack = *attack_opt;
    if (attack.source_frame_index == combat_state_.committed_attack_last_dispatched_frame_index &&
        attack.attack_payload_id == combat_state_.committed_attack_last_payload_id) {
        return;
    }

    bool one_shot_attack = false;
    auto anim_it = self_->info->animations.find(self_->current_animation);
    if (anim_it != self_->info->animations.end()) {
        one_shot_attack =
            animation_update::tag_utils::has_normalized_tag(anim_it->second.tags, "die");
    }

    target->send_attack(attack);
    if (debug_enabled_) {
        const std::string target_name =
            (target->info && !target->info->name.empty()) ? target->info->name : std::string{"<unknown>"};
        vibble::log::info("[AICombat] Attack hit dispatched to '" + target_name +
                          "' payload='" + attack.attack_payload_id +
                          "' frame=" + std::to_string(attack.source_frame_index) +
                          " damage=" + std::to_string(attack.payload.damage_amount) +
                          " knockback=" + std::to_string(attack.payload.hitback_enabled));
    }
    combat_state_.committed_attack_last_dispatched_frame_index = attack.source_frame_index;
    combat_state_.committed_attack_last_payload_id = attack.attack_payload_id;
    if (one_shot_attack) {
        clear_attack_commitment();
    }
}

Asset* AnimationRuntime::resolve_fallback_attack_target() {
    const auto candidates = attack_candidate_targets();
    if (candidates.empty()) {
        return nullptr;
    }
    Asset* player = assets_owner_ ? assets_owner_->game_context().player() : nullptr;
    if (!player && assets_owner_) {
        player = assets_owner_->player;
    }
    auto valid = [](Asset* a) {
        return a && a->active && !a->dead && a->isHitboxEnabled();
    };
    if (valid(player) &&
        std::find(candidates.begin(), candidates.end(), player) != candidates.end()) {
        return player;
    }
    for (Asset* candidate : candidates) {
        if (valid(candidate)) {
            return candidate;
        }
    }
    return nullptr;
}

void AnimationRuntime::refresh_runtime_frame_geometry() {
    if (!self_) {
        return;
    }

    self_->update_anchor_basis_if_needed();
    self_->refresh_anchor_point_cache_from_frame();
    self_->refresh_runtime_box_cache_from_frame();
}

bool AnimationRuntime::committed_attack_execution_active() const {
    if (!self_ || !self_->info) {
        return false;
    }
    const auto it = self_->info->animations.find(self_->current_animation);
    if (it == self_->info->animations.end()) {
        return false;
    }
    const Animation& anim = it->second;
    return anim.locked && animation_is_attack_candidate(anim);
}

animation_update::detail::PathBlockingContext AnimationRuntime::active_path_blocking_context() const {
    animation_update::detail::PathBlockingContext context;
    if (!planner_iface_) {
        return context;
    }
    if (planner_iface_->active_plan_mode_ == AnimationUpdate::ActivePlanMode::Plan2D) {
        context.engagement_target_asset_id = planner_iface_->plan_.engagement_target_asset_id;
    } else if (planner_iface_->active_plan_mode_ == AnimationUpdate::ActivePlanMode::Plan3D) {
        context.engagement_target_asset_id = planner_iface_->plan3d_.engagement_target_asset_id;
    }
    context.allow_engagement_target_overlap =
        context.engagement_target_asset_id.has_value() && committed_attack_execution_active();
    return context;
}

void AnimationRuntime::set_planner(AnimationUpdate* planner) {
    if (planner_iface_ && planner_iface_ != planner) {
        planner_iface_->set_runtime(nullptr);
    }
    planner_iface_ = planner;
    if (!planner_iface_) {
        clear_attack_commitment();
    }
    if (planner_iface_) {
        planner_iface_->set_runtime(this);
    }
}

float AnimationRuntime::parent_world_z() const {
    if (!self_ || !assets_owner_) {
        return 0.0f;
    }
    const WarpedScreenGrid& cam = assets_owner_->getView();
    if (const auto* gp = cam.grid_point_for_asset(self_)) {
        return static_cast<float>(gp->world_z());
    }
    return 0.0f;
}

void AnimationRuntime::set_debug_enabled(bool enabled) {
    debug_enabled_ = enabled;
}

void AnimationRuntime::clear_reverse_playback_state() {
    PlaybackController::clear_reverse(*this);
}

bool AnimationRuntime::reverse_mode_applies_to_current_animation() const {
    return playback_state_.reverse_mode != ReversePlaybackMode::None &&
           !playback_state_.reverse_animation_id.empty() &&
           self_ &&
           self_->current_animation == playback_state_.reverse_animation_id;
}

void AnimationRuntime::activate_reverse_playback(ReversePlaybackMode mode) {
    if (!self_ || !self_->info) {
        return;
    }

    auto it = self_->info->animations.find(self_->current_animation);
    if (it == self_->info->animations.end()) {
        return;
    }

    Animation& anim = it->second;
    if (!self_->current_frame) {
        self_->current_frame = anim.get_first_frame(path_index_for(self_->current_animation));
    }
    if (!self_->current_frame) {
        return;
    }

    playback_state_.reverse_mode = mode;
    playback_state_.reverse_animation_id = self_->current_animation;
    playback_state_.lock_on_end_active = false;
    self_->static_frame = false;
    self_->mark_composite_dirty();
    self_->mark_anchors_dirty();
}

void AnimationRuntime::begin_reverse_current_animation_until_stop() {
    activate_reverse_playback(ReversePlaybackMode::ReverseUntilStopCurrentAnimation);
}

void AnimationRuntime::begin_reverse_current_animation_to_default() {
    activate_reverse_playback(ReversePlaybackMode::ReverseToDefaultAtStart);
}

void AnimationRuntime::stop_reverse_current_animation() {
    clear_reverse_playback_state();
}

AnimationFrame* AnimationRuntime::last_frame_for(const Animation& anim, std::size_t path_index) const {
    const auto& path = anim.movement_path(path_index);
    if (path.empty()) {
        return nullptr;
    }
    return const_cast<AnimationFrame*>(&path.back());
}

void AnimationRuntime::update() {
    if (!self_ || !self_->info || !planner_iface_) {
        return;
    }

    struct SuppressionDecay {
        AnimationRuntime* runtime = nullptr;
        ~SuppressionDecay() {
            if (runtime && runtime->suppress_root_motion_frames_ > 0) {
                --runtime->suppress_root_motion_frames_;
            }
        }
    } decay{ this };
    std::uint64_t cycle_boundary_observed = 0;
    std::uint64_t attack_trigger_committed = 0;
    auto& frame_stats = runtime_stats::FrameStatsRecorder::instance();

    const bool freeze_for_frame_editor =
        assets_owner_ && assets_owner_->is_frame_editor_target_active(self_);
    const bool movement_blocked_for_dev_mode =
        assets_owner_ && !runtime::dev_mode_policy::should_allow_movement_for_asset(assets_owner_->is_dev_mode());
    const bool movement_disabled_for_asset = !self_->isMovementEnabled();

    if (freeze_for_frame_editor || movement_blocked_for_dev_mode || movement_disabled_for_asset) {
        const bool has_plan_2d =
            planner_iface_->active_plan_mode_ == AnimationUpdate::ActivePlanMode::Plan2D &&
            !planner_iface_->plan_.strides.empty();
        const bool has_plan_3d =
            planner_iface_->active_plan_mode_ == AnimationUpdate::ActivePlanMode::Plan3D &&
            !planner_iface_->plan3d_.strides.empty();
        const bool has_plan = has_plan_2d || has_plan_3d;
        if (has_plan || planner_iface_->has_pending_move() || planner_iface_->has_pending_move_3d()) {
            planner_iface_->clear_movement_plan();
            planner_iface_->move_pending_ = false;
            planner_iface_->pending_move_ = {};
            planner_iface_->move_pending_3d_ = false;
            planner_iface_->pending_move_3d_ = {};
        }
    }

    if (freeze_for_frame_editor) {
        dispatch_active_attack_payload();
        return;
    }

    if (movement_blocked_for_dev_mode) {
        dispatch_active_attack_payload();
        return;
    }

    if (!freeze_for_frame_editor && !movement_blocked_for_dev_mode && !movement_disabled_for_asset) {
        (void)planner_iface_->consume_input_event();
    }

    if (!freeze_for_frame_editor && !movement_blocked_for_dev_mode && !movement_disabled_for_asset) {
        if (planner_iface_->active_plan_mode_ == AnimationUpdate::ActivePlanMode::Plan2D &&
            planner_iface_->plan_.strides.empty()) {
            planner_iface_->active_plan_mode_ = AnimationUpdate::ActivePlanMode::None;
        }
        if (planner_iface_->active_plan_mode_ == AnimationUpdate::ActivePlanMode::Plan3D &&
            planner_iface_->plan3d_.strides.empty()) {
            planner_iface_->active_plan_mode_ = AnimationUpdate::ActivePlanMode::None;
        }

        const bool has_plan_2d =
            planner_iface_->active_plan_mode_ == AnimationUpdate::ActivePlanMode::Plan2D &&
            !planner_iface_->plan_.strides.empty();
        const bool has_plan_3d =
            planner_iface_->active_plan_mode_ == AnimationUpdate::ActivePlanMode::Plan3D &&
            !planner_iface_->plan3d_.strides.empty();
        const bool has_plan = has_plan_2d || has_plan_3d;
        const bool plan_override_non_locked =
            has_plan_3d ? planner_iface_->plan3d_.override_non_locked : planner_iface_->plan_.override_non_locked;
        const bool plan_deferred = has_plan && should_defer_for_non_locked(plan_override_non_locked);

        if (has_plan && !plan_deferred) {
            const bool consumed = has_plan_3d
                ? executor_.tick_3d(*this, planner_iface_->plan3d_, stride_index_, stride_frame_counter_)
                : executor_.tick(*this, planner_iface_->plan_, stride_index_, stride_frame_counter_);
            if (consumed) {
                frame_stats.set("combat.attack_cycle_boundary_observed", cycle_boundary_observed);
                frame_stats.set("combat.attack_cycle_trigger_committed", attack_trigger_committed);
                dispatch_active_attack_payload();
                return;
            }
        }

        if (planner_iface_->has_pending_move()) {
            const auto& req = planner_iface_->pending_move_;
            if (!should_defer_for_non_locked(req.override_non_locked)) {
                apply_pending_move();
                frame_stats.set("combat.attack_cycle_boundary_observed", cycle_boundary_observed);
                frame_stats.set("combat.attack_cycle_trigger_committed", attack_trigger_committed);
                dispatch_active_attack_payload();
                return;
            }
        }
        if (planner_iface_->has_pending_move_3d()) {
            const auto& req3d = planner_iface_->pending_move_3d_;
            if (!should_defer_for_non_locked(req3d.override_non_locked)) {
                apply_pending_move_3d();
                frame_stats.set("combat.attack_cycle_boundary_observed", cycle_boundary_observed);
                frame_stats.set("combat.attack_cycle_trigger_committed", attack_trigger_committed);
                dispatch_active_attack_payload();
                return;
            }
        }
    }

    const bool cycle_boundary_before_advance =
        self_->current_frame && self_->current_frame->next == nullptr;
    (void)advance(self_->current_frame);
    (void)process_cycle_boundary_event(cycle_boundary_before_advance,
                                       cycle_boundary_observed,
                                       attack_trigger_committed);
    frame_stats.set("combat.attack_cycle_boundary_observed", cycle_boundary_observed);
    frame_stats.set("combat.attack_cycle_trigger_committed", attack_trigger_committed);
    dispatch_active_attack_payload();
}

void AnimationRuntime::apply_pending_move() {
    if (!planner_iface_ || !self_) return;

    const auto req = planner_iface_->consume_move_request();
    const int  resolution = effective_grid_resolution(std::nullopt);
    const SDL_Point from{ self_->world_x(), self_->world_z() };
    SDL_Point world_delta = convert_delta_to_world(req.delta, resolution);
    const SDL_Point to{ from.x + world_delta.x, from.y + world_delta.y };

    SDL_Point final_position = from;
    const auto block_context = active_path_blocking_context();
    auto& frame_stats = runtime_stats::FrameStatsRecorder::instance();
    std::uint64_t path_blocked_checks = 0;
    std::uint64_t fallback_probes = 0;
    double path_blocked_ms = 0.0;
    double fallback_ms = 0.0;
    bool direct_blocked = false;
    bool fallback_used = false;
    bool fallback_probe_cap_hit = false;

    auto timed_path_blocked = [&](SDL_Point check_from,
                                  SDL_Point check_to,
                                  std::vector<const Asset*>* blockers = nullptr) {
        const Uint64 begin = SDL_GetPerformanceCounter();
        const bool blocked = path_blocked(check_from, check_to, self_, blockers, block_context);
        path_blocked_ms += runtime_stats::FrameStatsRecorder::elapsed_ms(begin, SDL_GetPerformanceCounter());
        ++path_blocked_checks;
        return blocked;
    };

    auto largest_safe_position_for_delta = [&](SDL_Point delta, bool full_move_known_blocked) {
        const int steps = std::max(std::abs(delta.x), std::abs(delta.y));
        if (steps <= 0) {
            return from;
        }

        const SDL_Point full_position{from.x + delta.x, from.y + delta.y};
        if (!full_move_known_blocked && !timed_path_blocked(from, full_position)) {
            return full_position;
        }

        SDL_Point best_position = from;
        int low = 0;
        int high = steps;
        while (low + 1 < high && fallback_probes < kMaxBlockedMoveFallbackProbes) {
            const int mid = low + (high - low) / 2;
            const SDL_Point candidate = point_on_delta(from, delta, mid, steps);
            if (candidate.x == best_position.x && candidate.y == best_position.y) {
                low = mid;
                continue;
            }

            ++fallback_probes;
            if (timed_path_blocked(from, candidate)) {
                high = mid;
            } else {
                low = mid;
                best_position = candidate;
            }
        }

        if (low + 1 < high) {
            fallback_probe_cap_hit = true;
        }
        return best_position;
    };

    auto progress_distance_sq = [&](SDL_Point candidate) {
        const int dx = candidate.x - from.x;
        const int dy = candidate.y - from.y;
        return dx * dx + dy * dy;
    };

    bool fallback_shortened_used = false;
    bool slide_used = false;
    bool unstick_used = false;

    if (world_delta.x != 0 || world_delta.y != 0) {
        std::vector<const Asset*> direct_blockers;
        direct_blocked = timed_path_blocked(from, to, &direct_blockers);
        if (!direct_blocked) {
            final_position = to;
        } else {
            fallback_used = true;
            const Uint64 fallback_begin = SDL_GetPerformanceCounter();

            final_position = largest_safe_position_for_delta(world_delta, true);
            fallback_shortened_used =
                final_position.x != from.x || final_position.y != from.y;

            if (!fallback_shortened_used && world_delta.x != 0 && world_delta.y != 0) {
                const SDL_Point x_position =
                    largest_safe_position_for_delta(SDL_Point{world_delta.x, 0}, false);
                const SDL_Point z_position =
                    largest_safe_position_for_delta(SDL_Point{0, world_delta.y}, false);
                const int x_progress = progress_distance_sq(x_position);
                const int z_progress = progress_distance_sq(z_position);
                const bool prefer_x_on_tie = std::abs(world_delta.x) >= std::abs(world_delta.y);
                if (x_progress > 0 || z_progress > 0) {
                    if (x_progress > z_progress || (x_progress == z_progress && prefer_x_on_tie)) {
                        final_position = x_position;
                    } else {
                        final_position = z_position;
                    }
                    slide_used = true;
                }
            }

            if (final_position.x == from.x && final_position.y == from.y) {
                unstick_used = attempt_unstick(from, to, direct_blockers);
                if (unstick_used) {
                    final_position = SDL_Point{self_->world_x(), self_->world_z()};
                }
            }

            fallback_ms = runtime_stats::FrameStatsRecorder::elapsed_ms(fallback_begin,
                                                                        SDL_GetPerformanceCounter());
        }
    }
    frame_stats.set("movement.player_delta_world_x", world_delta.x);
    frame_stats.set("movement.player_delta_world_z", world_delta.y);
    frame_stats.set("movement.player_final_delta_world_x", final_position.x - from.x);
    frame_stats.set("movement.player_final_delta_world_z", final_position.y - from.y);
    frame_stats.set("movement.player_path_blocked_checks", path_blocked_checks);
    frame_stats.set("movement.player_path_blocked_ms", path_blocked_ms);
    frame_stats.set("movement.player_direct_blocked", direct_blocked);
    frame_stats.set("movement.player_fallback_used", fallback_used);
    frame_stats.set("movement.player_fallback_probes", fallback_probes);
    frame_stats.set("movement.player_fallback_ms", fallback_ms);
    frame_stats.set("movement.player_fallback_probe_cap_hit", fallback_probe_cap_hit);
    frame_stats.set("movement.player_fallback_shortened_used", fallback_shortened_used);
    frame_stats.set("movement.player_slide_used", slide_used);
    frame_stats.set("movement.player_unstick_used", unstick_used);

    if (final_position.x != self_->world_x() || final_position.y != self_->world_z()) {
        self_->move_to_world_position(final_position.x, self_->world_y(), final_position.y);
        suppress_root_motion_frames_ = std::max(2, suppress_root_motion_frames_);
        if (planner_iface_) {
            planner_iface_->clear_movement_plan();
        }
    }

    planner_iface_->active_plan_mode_ = AnimationUpdate::ActivePlanMode::None;
    planner_iface_->final_dest = self_->world_xz_point();
    planner_iface_->final_dest_3d = axis::WorldPos{ self_->world_x(), self_->world_y(), self_->world_z() };

    const std::string resolved = resolve_animation(*self_, req.animation_id);
    if (self_->current_animation != resolved) {
        switch_to(resolved, path_index_for(resolved), TransitionLockPolicy::Force);
    } else {
        if (!advance(self_->current_frame)) {
            if (self_->dead) {
                return;
            }
            switch_to(resolved, path_index_for(resolved), TransitionLockPolicy::Force);
        }
    }

    switch (req.reverse_command) {
    case AnimationUpdate::ReversePlaybackCommand::ReverseUntilStopCurrentAnimation:
        begin_reverse_current_animation_until_stop();
        break;
    case AnimationUpdate::ReversePlaybackCommand::ReverseToDefaultAtStart:
        begin_reverse_current_animation_to_default();
        break;
    case AnimationUpdate::ReversePlaybackCommand::Stop:
        stop_reverse_current_animation();
        break;
    case AnimationUpdate::ReversePlaybackCommand::None:
    default:
        break;
    }
}

void AnimationRuntime::apply_pending_move_3d() {
    if (!planner_iface_ || !self_) {
        return;
    }

    const auto req = planner_iface_->consume_move_request_3d();
    const int resolution = effective_grid_resolution(std::nullopt);
    (void)resolution;
    const axis::WorldPos from{ self_->world_x(), self_->world_y(), self_->world_z() };
    const axis::WorldPos world_delta = req.delta;
    const axis::WorldPos to{
        from.x + world_delta.x,
        from.y + world_delta.y,
        from.z + world_delta.z
    };

    axis::WorldPos final_position = from;
    const auto block_context = active_path_blocking_context();
    if (world_delta.x != 0 || world_delta.y != 0 || world_delta.z != 0) {
        const world::GridPoint gp_from = world::GridPoint::make_virtual(from.x, from.y, from.z, self_->grid_resolution);
        const world::GridPoint gp_to = world::GridPoint::make_virtual(to.x, to.y, to.z, self_->grid_resolution);
        if (!path_blocked(gp_from, gp_to, self_, nullptr, block_context)) {
            final_position = to;
        } else {
            const int steps = std::max({ std::abs(world_delta.x), std::abs(world_delta.y), std::abs(world_delta.z) });
            if (steps > 0) {
                const double step_x = static_cast<double>(world_delta.x) / static_cast<double>(steps);
                const double step_y = static_cast<double>(world_delta.y) / static_cast<double>(steps);
                const double step_z = static_cast<double>(world_delta.z) / static_cast<double>(steps);
                double accum_x = static_cast<double>(from.x);
                double accum_y = static_cast<double>(from.y);
                double accum_z = static_cast<double>(from.z);
                axis::WorldPos current = from;
                for (int i = 0; i < steps; ++i) {
                    accum_x += step_x;
                    accum_y += step_y;
                    accum_z += step_z;
                    const axis::WorldPos candidate{
                        static_cast<int>(std::round(accum_x)),
                        static_cast<int>(std::round(accum_y)),
                        static_cast<int>(std::round(accum_z))
                    };
                    if (candidate.x == current.x && candidate.y == current.y && candidate.z == current.z) {
                        continue;
                    }
                    const world::GridPoint current_gp =
                        world::GridPoint::make_virtual(current.x, current.y, current.z, self_->grid_resolution);
                    const world::GridPoint candidate_gp =
                        world::GridPoint::make_virtual(candidate.x, candidate.y, candidate.z, self_->grid_resolution);
                    if (path_blocked(current_gp, candidate_gp, self_, nullptr, block_context)) {
                        break;
                    }
                    final_position = candidate;
                    current = candidate;
                }
            }
        }
    }

    if (final_position.x != self_->world_x() ||
        final_position.y != self_->world_y() ||
        final_position.z != self_->world_z()) {
        self_->move_to_world_position(final_position.x, final_position.y, final_position.z);
        suppress_root_motion_frames_ = std::max(2, suppress_root_motion_frames_);
        if (planner_iface_) {
            planner_iface_->clear_movement_plan();
        }
    }

    planner_iface_->active_plan_mode_ = AnimationUpdate::ActivePlanMode::None;
    planner_iface_->final_dest = self_->world_xz_point();
    planner_iface_->final_dest_3d = axis::WorldPos{ self_->world_x(), self_->world_y(), self_->world_z() };

    const std::string resolved = resolve_animation(*self_, req.animation_id);
    if (self_->current_animation != resolved) {
        switch_to(resolved, path_index_for(resolved), TransitionLockPolicy::Force);
    } else {
        if (!advance(self_->current_frame)) {
            if (self_->dead) {
                return;
            }
            switch_to(resolved, path_index_for(resolved), TransitionLockPolicy::Force);
        }
    }
}

AnimationRuntime::FrameAdvanceReport AnimationRuntime::advance_with_report(AnimationFrame*& frame, int max_events) {
    FrameAdvanceReport report{};
    std::uint64_t root_motion_applied = 0;
    std::uint64_t root_motion_blocked = 0;
    if (!self_ || !self_->info) {
        report.ok = false;
        return report;
    }

    auto it = self_->info->animations.find(self_->current_animation);
    if (it == self_->info->animations.end()) {
        report.ok = false;
        return report;
    }

    Animation* anim = &it->second;
    std::size_t path_index = path_index_for(self_->current_animation);
    auto record_entered_frame = [&](bool cycle_boundary_before_advance) {
        report.advanced_any = true;
        report.entered_frames.push_back(FrameAdvanceEvent{
            frame,
            self_ ? self_->current_animation : std::string{},
            path_index,
            cycle_boundary_before_advance
        });
    };
    if (!reverse_mode_applies_to_current_animation()) {
        clear_reverse_playback_state();
    }
    if (!frame) {
        frame = anim->get_first_frame(path_index);
        if (!frame) {
            report.ok = false;
            return report;
        }
    }

    const bool is_player = self_->info && self_->info->type == asset_types::player;
    const bool reverse_command_active = reverse_mode_applies_to_current_animation();
    const bool attack_follow_through =
        current_animation_is_attack() &&
        (auto_attack_commitment_active() || committed_attack_execution_active());
    const bool static_blocked = self_->static_frame && !reverse_command_active && !attack_follow_through;
    bool should_skip = !is_player && (static_blocked || anim->is_frozen() || playback_state_.lock_on_end_active);
    bool has_overriding_plan = false;
    if (planner_iface_) {
        if (planner_iface_->active_plan_mode_ == AnimationUpdate::ActivePlanMode::Plan2D) {
            has_overriding_plan =
                !planner_iface_->plan_.strides.empty() && planner_iface_->plan_.override_non_locked;
        } else if (planner_iface_->active_plan_mode_ == AnimationUpdate::ActivePlanMode::Plan3D) {
            has_overriding_plan =
                !planner_iface_->plan3d_.strides.empty() && planner_iface_->plan3d_.override_non_locked;
        }
    }
    if (should_skip && !has_overriding_plan) {
        self_->static_frame = self_->static_frame || anim->is_frozen() || playback_state_.lock_on_end_active;
        return report;
    }
    if (is_player) {
        self_->static_frame = false;
    }

    constexpr int target_fps = kBaseAnimationFps;
    const float frame_interval = 1.0f / static_cast<float>(target_fps);
    float dt = 0.0f;
    if (assets_owner_) {
        dt = assets_owner_->frame_delta_seconds();
    }
    if (!(dt > 0.0f)) {
        dt = 1.0f / 60.0f;
    }

    self_->frame_progress += dt;
    while (self_->frame_progress >= frame_interval &&
           (max_events < 0 || static_cast<int>(report.entered_frames.size()) < max_events)) {
        self_->frame_progress -= frame_interval;
        const bool cycle_boundary_before_advance =
            frame && frame->next == nullptr;

        if (reverse_mode_applies_to_current_animation()) {
            if (frame->prev) {
                frame = frame->prev;
                record_entered_frame(false);
                continue;
            }

            if (playback_state_.reverse_mode == ReversePlaybackMode::ReverseUntilStopCurrentAnimation) {
                frame = last_frame_for(*anim, path_index);
                if (!frame) {
                    report.ok = false;
                    return report;
                }
                record_entered_frame(false);
                continue;
            }

            const ReversePlaybackMode mode_at_boundary = playback_state_.reverse_mode;
            clear_reverse_playback_state();
            if (mode_at_boundary == ReversePlaybackMode::ReverseToDefaultAtStart) {
                switch_to(animation_update::detail::kDefaultAnimation,
                          path_index_for(animation_update::detail::kDefaultAnimation),
                          TransitionLockPolicy::Force);
                frame = self_->current_frame;
                if (!frame) {
                    report.ok = false;
                    return report;
                }
                it = self_->info->animations.find(self_->current_animation);
                if (it == self_->info->animations.end()) {
                    report.ok = false;
                    return report;
                }
                anim = &it->second;
                path_index = path_index_for(self_->current_animation);
                record_entered_frame(cycle_boundary_before_advance);
                continue;
            }

            continue;
        }

        if (frame->next) {
            frame = frame->next;
            record_entered_frame(false);
            if (!apply_frame_root_motion_delta(frame, root_motion_applied, root_motion_blocked)) {
                report.ok = false;
                return report;
            }
            continue;
        }

        const Animation::OnEndDirective directive = anim->on_end_behavior;
        switch (directive) {
        case Animation::OnEndDirective::Loop: {
            if (current_animation_is_attack() &&
                combat_state_.committed_attack_target_asset_id.has_value()) {
                clear_attack_commitment();
                switch_to(animation_update::detail::kDefaultAnimation,
                          path_index_for(animation_update::detail::kDefaultAnimation),
                          TransitionLockPolicy::Force);
                if (self_) {
                    self_->needs_target = true;
                }
                frame = self_->current_frame;
                if (!frame) {
                    report.ok = false;
                    return report;
                }
                it = self_->info->animations.find(self_->current_animation);
                if (it == self_->info->animations.end()) {
                    report.ok = false;
                    return report;
                }
                anim = &it->second;
                path_index = path_index_for(self_->current_animation);
                record_entered_frame(cycle_boundary_before_advance);
                if (!apply_frame_root_motion_delta(frame, root_motion_applied, root_motion_blocked)) {
                    report.ok = false;
                    return report;
                }
                break;
            }
            frame = anim->get_first_frame(path_index);
            if (!frame) {
                report.ok = false;
                return report;
            }
            record_entered_frame(cycle_boundary_before_advance);
            if (!apply_frame_root_motion_delta(frame, root_motion_applied, root_motion_blocked)) {
                report.ok = false;
                return report;
            }
            break;
        }
        case Animation::OnEndDirective::Kill:
            self_->Delete();
            report.ok = false;
            return report;
        case Animation::OnEndDirective::Lock:
            playback_state_.lock_on_end_active = true;
            self_->static_frame = true;
            if (debug_enabled_) {
                vibble::log::info("[AICombat] Animation lock engaged on end: '" + self_->current_animation + "'");
            }
            return report;
        case Animation::OnEndDirective::Reverse:
            activate_reverse_playback(ReversePlaybackMode::ReverseToDefaultAtStart);
            playback_state_.lock_on_end_active = false;
            self_->static_frame = false;
            if (debug_enabled_) {
                vibble::log::info("[AICombat] Animation reverse-on-end requested for '" + self_->current_animation + "'");
            }
            break;
        case Animation::OnEndDirective::Animation: {
            const std::string requested = anim->on_end_animation.empty()
                                              ? std::string{animation_update::detail::kDefaultAnimation}
                                              : anim->on_end_animation;
            const std::string resolved = resolve_animation(*self_, requested);
            if (current_animation_is_attack()) {
                combat_state_.attack_recovery_pending = true;
                combat_state_.attack_recovery_animation_id = resolved;
            }
            switch_to(resolved, path_index_for(requested), TransitionLockPolicy::Force);
            frame = self_->current_frame;
            if (!frame) {
                report.ok = false;
                return report;
            }
            it = self_->info->animations.find(self_->current_animation);
            if (it == self_->info->animations.end()) {
                report.ok = false;
                return report;
            }
            anim = &it->second;
            path_index = path_index_for(self_->current_animation);
            record_entered_frame(cycle_boundary_before_advance);
            if (!apply_frame_root_motion_delta(frame, root_motion_applied, root_motion_blocked)) {
                report.ok = false;
                return report;
            }
            break;
        }
        case Animation::OnEndDirective::Default:
        default: {
            const bool completed_committed_attack =
                current_animation_is_attack() &&
                combat_state_.committed_attack_target_asset_id.has_value();
            switch_to(animation_update::detail::kDefaultAnimation,
                      path_index_for(animation_update::detail::kDefaultAnimation),
                      TransitionLockPolicy::Force);
            if (completed_committed_attack && self_) {
                self_->needs_target = true;
            }
            frame = self_->current_frame;
            if (!frame) {
                report.ok = false;
                return report;
            }
            it = self_->info->animations.find(self_->current_animation);
            if (it == self_->info->animations.end()) {
                report.ok = false;
                return report;
            }
            anim = &it->second;
            path_index = path_index_for(self_->current_animation);
            record_entered_frame(cycle_boundary_before_advance);
            if (!apply_frame_root_motion_delta(frame, root_motion_applied, root_motion_blocked)) {
                report.ok = false;
                return report;
            }
            break;
        }
        }
    }
    if (report.advanced_any) {
        self_->mark_composite_dirty();
        self_->mark_anchors_dirty();
        refresh_runtime_frame_geometry();
    }
    runtime_stats::FrameStatsRecorder::instance().set("movement.attack_root_motion_applied", root_motion_applied);
    runtime_stats::FrameStatsRecorder::instance().set("movement.attack_root_motion_blocked", root_motion_blocked);
    return report;
}

bool AnimationRuntime::advance(AnimationFrame*& frame) {
    return advance_with_report(frame).ok;
}

bool AnimationRuntime::can_interrupt_current_animation(TransitionLockPolicy lock_policy,
                                                       const std::string& target_anim_id) const {
    if (lock_policy == TransitionLockPolicy::Force || !self_ || !self_->info) {
        return true;
    }
    if (self_->current_animation == target_anim_id) {
        return true;
    }
    if (self_->current_frame && self_->current_frame->is_last) {
        return true;
    }

    const auto current_it = self_->info->animations.find(self_->current_animation);
    if (current_it == self_->info->animations.end()) {
        return true;
    }
    return !current_it->second.locked;
}

bool AnimationRuntime::switch_to(const std::string& anim_id,
                                 std::size_t path_index,
                                 TransitionLockPolicy lock_policy) {
    if (!self_ || !self_->info) {
        return false;
    }
    if (!can_interrupt_current_animation(lock_policy, anim_id)) {
        if (debug_enabled_) {
            vibble::log::info("[AICombat] Animation switch to '" + anim_id +
                              "' deferred because current animation '" + self_->current_animation +
                              "' is locked");
        }
        return false;
    }

    clear_reverse_playback_state();
    const bool was_locked_on_end = playback_state_.lock_on_end_active;
    playback_state_.lock_on_end_active = false;
    if (debug_enabled_ && was_locked_on_end) {
        vibble::log::info("[AICombat] Animation lock released during switch_to()");
    }

    auto it = self_->info->animations.find(anim_id);
    if (it == self_->info->animations.end()) {
        auto def = self_->info->animations.find(animation_update::detail::kDefaultAnimation);
        if (def == self_->info->animations.end()) {
            if (self_->info->animations.empty()) {
                return false;
            }
            it = self_->info->animations.begin();
        } else {
            it = def;
        }
    }

    const std::string previous_animation = self_->current_animation;
    bool previously_attack = false;
    auto previous_it = self_->info->animations.find(previous_animation);
    if (previous_it != self_->info->animations.end()) {
        previously_attack = animation_is_attack_candidate(previous_it->second);
    }

    Animation& anim = it->second;
    path_index = anim.clamp_path_index(path_index);
    AnimationFrame* new_frame = anim.get_first_frame(path_index);
    self_->current_animation = it->first;
    self_->current_frame     = new_frame;
    if (combat_state_.attack_recovery_pending &&
        self_->current_animation != combat_state_.attack_recovery_animation_id) {
        combat_state_.attack_recovery_pending = false;
        combat_state_.attack_recovery_animation_id.clear();
    }
    const bool switched_to_attack = animation_is_attack_candidate(anim);
    if (!switched_to_attack) {
        if (debug_enabled_ && previously_attack) {
            vibble::log::info("[AICombat] Attack animation ended; switching to '" + self_->current_animation + "'");
        }
        clear_attack_commitment();
    } else if (combat_state_.committed_attack_animation_id != self_->current_animation) {
        combat_state_.committed_attack_animation_id = self_->current_animation;
        combat_state_.committed_attack_path_index = path_index;
        combat_state_.committed_attack_last_dispatched_frame_index = -1;
        combat_state_.committed_attack_last_payload_id.clear();
        if (debug_enabled_) {
            vibble::log::info("[AICombat] Attack animation start: '" + self_->current_animation + "'");
        }
    } else {
        combat_state_.committed_attack_path_index = path_index;
    }
    {
        const bool is_player = self_->info && self_->info->type == asset_types::player;
        self_->static_frame  = is_player ? false : anim.is_frozen();
    }
    self_->frame_progress    = 0.0f;
    active_paths_[self_->current_animation] = path_index;
    self_->mark_composite_dirty();
    self_->mark_anchors_dirty();
    refresh_runtime_frame_geometry();
    return true;
}

bool AnimationRuntime::should_defer_for_non_locked(bool override_non_locked) const {
    // Lock semantics contract:
    // - Locked animations are protected from movement/pending-request interruption unless
    //   the caller explicitly opts into override_non_locked.
    // - Non-locked animations may defer movement transitions unless the caller explicitly
    //   opts into override_non_locked (e.g., authored interrupt behavior).
    // - Default idle may always transition.
    if (!self_ || !self_->info) {
        return false;
    }

    auto it = self_->info->animations.find(self_->current_animation);
    if (it == self_->info->animations.end()) {
        return false;
    }

    if (self_->current_animation == animation_update::detail::kDefaultAnimation) {
        return false;
    }

    if (override_non_locked) {
        return false;
    }

    return true;
}

bool AnimationRuntime::process_cycle_boundary_event(bool cycle_boundary_before_advance,
                                                    std::uint64_t& boundary_observed_counter,
                                                    std::uint64_t& attack_trigger_committed_counter) {
    if (!cycle_boundary_before_advance) {
        return false;
    }
    ++boundary_observed_counter;
    const bool triggered = maybe_trigger_attack_on_cycle_boundary();
    if (triggered) {
        ++attack_trigger_committed_counter;
    }
    return triggered;
}

bool AnimationRuntime::apply_frame_root_motion_delta(AnimationFrame* frame,
                                                     std::uint64_t& applied_counter,
                                                     std::uint64_t& blocked_counter) {
    if (!self_ || !frame) {
        return true;
    }
    if (suppress_root_motion_active()) {
        return true;
    }
    const axis::WorldPos delta = animation_update::detail::frame_world_delta_3d(*frame, *self_, grid());
    if (delta.x == 0 && delta.y == 0 && delta.z == 0) {
        return true;
    }

    const axis::WorldPos from{self_->world_x(), self_->world_y(), self_->world_z()};
    const axis::WorldPos to{from.x + delta.x, from.y + delta.y, from.z + delta.z};
    const auto block_context = active_path_blocking_context();
    if ((delta.x != 0 || delta.z != 0) &&
        path_blocked(world::GridPoint::make_virtual(from.x, from.y, from.z, self_->grid_resolution),
                     world::GridPoint::make_virtual(to.x, to.y, to.z, self_->grid_resolution),
                     self_,
                     nullptr,
                     block_context)) {
        ++blocked_counter;
        return true;
    }
    self_->move_to_world_position(to.x, to.y, to.z);
    ++applied_counter;
    return true;
}

std::size_t AnimationRuntime::path_index_for(const std::string& anim_id) const {
    auto it = active_paths_.find(anim_id);
    if (it != active_paths_.end()) {
        return it->second;
    }
    return 0;
}

void AnimationRuntime::reset_plan_progress() {
    stride_index_ = 0;
    stride_frame_counter_ = 0;
    next_checkpoint_index_ = 0;
}

world::GridPoint AnimationRuntime::bottom_middle(const world::GridPoint& pos) const {
    if (!self_ || !self_->info) {
        return pos;
    }
    return animation_update::detail::bottom_middle_for(*self_, pos);
}

SDL_Point AnimationRuntime::bottom_middle(SDL_Point pos) const {
    const int world_y = self_ ? self_->world_y() : 0;
    const int layer = self_ ? self_->grid_resolution : 0;
    return bottom_middle(world::grid_math::from_sdl(pos, world_y, layer)).to_sdl_point();
}

bool AnimationRuntime::point_in_impassable(const world::GridPoint& pt, const Asset* ignored) const {
    if (!self_ || !self_->info) {
        return false;
    }
    const Assets* assets = assets_owner_ ? assets_owner_ : (self_ ? self_->get_assets() : nullptr);
    if (!animation_update::detail::bottom_point_inside_playable_area(assets, pt)) {
        return true;
    }
    return visit_impassable_neighbors(*self_, [&](const Assets::FrameCollisionEntry* entry,
                                                  const Asset* neighbor,
                                                  const Area& area,
                                                  const world::GridPoint&) {
        if (!neighbor || neighbor == self_ || neighbor == ignored || !neighbor->info) {
            return false;
        }
        if (entry->canonical_type == asset_types::player) {
            return false;
        }
        return area.contains_point(pt.to_sdl_point());
    });
}

bool AnimationRuntime::point_in_impassable(SDL_Point pt, const Asset* ignored) const {
    const int world_y = self_ ? self_->world_y() : 0;
    const int layer = self_ ? self_->grid_resolution : 0;
    return point_in_impassable(world::grid_math::from_sdl(pt, world_y, layer), ignored);
}

bool AnimationRuntime::path_blocked(const world::GridPoint& from,
                                    const world::GridPoint& to,
                                    const Asset* ignored,
                                    std::vector<const Asset*>* blockers,
                                    const animation_update::detail::PathBlockingContext& context) const {
    if (!self_ || !self_->info) {
        return false;
    }
    const world::GridPoint bottom_from = animation_update::detail::bottom_middle_for(*self_, from);
    const world::GridPoint dest_bottom = animation_update::detail::bottom_middle_for(*self_, to);
    const Assets* assets = assets_owner_ ? assets_owner_ : (self_ ? self_->get_assets() : nullptr);
    if (animation_update::detail::segment_leaves_playable_area(assets, bottom_from, dest_bottom)) {
        return true;
    }
    bool blocked = false;
    visit_impassable_neighbors_for_segment(*self_,
                                           from.to_sdl_point(),
                                           to.to_sdl_point(),
                                           [&](const Assets::FrameCollisionEntry* entry,
                                               const Asset* neighbor,
                                               const Area& area,
                                               const world::GridPoint& neighbor_bottom) {
        if (!neighbor || neighbor == self_ || neighbor == ignored || !neighbor->info) {
            return false;
        }
        if (entry->canonical_type == asset_types::player) {
            return false;
        }
        const bool contains_from = area.contains_point(bottom_from.to_sdl_point());
        const bool contains_to   = area.contains_point(dest_bottom.to_sdl_point());
        const std::string neighbor_id = animation_update::detail::stable_asset_id(*neighbor);
        const bool is_engagement_target =
            context.allow_engagement_target_overlap &&
            context.engagement_target_asset_id.has_value() &&
            !neighbor_id.empty() &&
            neighbor_id == *context.engagement_target_asset_id;
        const bool touches_segment =
            !is_engagement_target && animation_update::detail::segment_hits_area(from, to, area);
        bool overlaps = false;
        if (!contains_from && !contains_to && !touches_segment) {
            const int overlap_distance_sq =
                animation_update::detail::overlap_distance_sq_for_pair(*self_, *neighbor, context);
            overlaps = overlap_distance_sq > 0 &&
                       animation_update::detail::distance_sq(dest_bottom, neighbor_bottom) < overlap_distance_sq;
        }
        if (!(contains_from || contains_to || touches_segment || overlaps)) {
            return false;
        }
        blocked = true;
        if (blockers) {
            const auto it = std::find(blockers->begin(), blockers->end(), neighbor);
            if (it == blockers->end()) {
                blockers->push_back(neighbor);
            }
        }
        return false;
    });
    return blocked;
}

bool AnimationRuntime::path_blocked(SDL_Point from,
                                    SDL_Point to,
                                    const Asset* ignored,
                                    std::vector<const Asset*>* blockers,
                                    const animation_update::detail::PathBlockingContext& context) const {
    const int world_y = self_ ? self_->world_y() : 0;
    const int layer = self_ ? self_->grid_resolution : 0;
    const world::GridPoint gp_from = world::grid_math::from_sdl(from, world_y, layer);
    const world::GridPoint gp_to   = world::grid_math::from_sdl(to, world_y, layer);
    return path_blocked(gp_from, gp_to, ignored, blockers, context);
}

bool AnimationRuntime::attempt_unstick(const world::GridPoint& from,
                                       const world::GridPoint& to,
                                       const std::vector<const Asset*>& blockers) {
    if (!self_ || !self_->info) {
        return false;
    }
    const Assets* assets = assets_owner_ ? assets_owner_ : (self_ ? self_->get_assets() : nullptr);
    std::vector<const Assets::FrameCollisionEntry*> runtime_entries;
    const long long dx = static_cast<long long>(to.world_x()) - static_cast<long long>(from.world_x());
    const long long dz = static_cast<long long>(to.world_z()) - static_cast<long long>(from.world_z());
    const int path_extent =
        static_cast<int>(std::lround(std::sqrt(static_cast<double>(dx * dx + dz * dz))));
    const int max_radius_cap = assets ? assets->max_impassable_query_radius() : kLocalImpassableQueryRadiusPx;
    const int search_radius = std::min(
        std::max(kUnstickMinQueryRadiusPx, path_extent + kUnstickRadiusPaddingPx),
        max_radius_cap);
    if (assets) {
        assets->query_impassable_entries(*self_, search_radius, runtime_entries);
        if (blockers.empty() && animation::unstick::push_out_of_impassable(*self_, assets, runtime_entries)) {
            return true;
        }
    }

    world::GridPoint bottom_from = animation_update::detail::bottom_middle_for(*self_, from);
    world::GridPoint bottom_to   = animation_update::detail::bottom_middle_for(*self_, to);
    const auto block_context = active_path_blocking_context();
    SDL_Point push{0, 0};
    std::vector<const Asset*> blocking_neighbors = blockers;
    if (blocking_neighbors.empty()) {
        visit_impassable_neighbors(*self_, [&](const Assets::FrameCollisionEntry*,
                                               const Asset* neighbor,
                                               const Area& area,
                                               const world::GridPoint& neighbor_bottom) {
            if (!neighbor || neighbor == self_ || !neighbor->info) {
                return false;
            }
            const bool contains_from = area.contains_point(bottom_from.to_sdl_point());
            const bool contains_to   = area.contains_point(bottom_to.to_sdl_point());
            const bool touches_segment = animation_update::detail::segment_hits_area(from, to, area);
            bool overlaps = false;
            if (!contains_from && !contains_to && !touches_segment) {
                const int overlap_distance_sq =
                    animation_update::detail::overlap_distance_sq_for_pair(*self_, *neighbor, block_context);
                overlaps = overlap_distance_sq > 0 &&
                           animation_update::detail::distance_sq(bottom_from, neighbor_bottom) < overlap_distance_sq;
            }
            if (!(contains_from || contains_to || touches_segment || overlaps)) {
                return false;
            }
            SDL_Point center = area.get_center();
            push.x += bottom_from.world_x() - center.x;
            push.y += bottom_from.world_z() - center.y;
            blocking_neighbors.push_back(neighbor);
            return false;
        });
    } else {
        std::unordered_set<const Asset*> blocker_lookup(blocking_neighbors.begin(), blocking_neighbors.end());
        visit_impassable_neighbors(*self_, [&](const Assets::FrameCollisionEntry*,
                                               const Asset* neighbor,
                                               const Area& area,
                                               const world::GridPoint&) {
            if (!neighbor || neighbor == self_ || !neighbor->info) {
                return false;
            }
            if (blocker_lookup.find(neighbor) == blocker_lookup.end()) {
                return false;
            }
            const SDL_Point center = area.get_center();
            push.x += bottom_from.world_x() - center.x;
            push.y += bottom_from.world_z() - center.y;
            return false;
        });
    }
    if (push.x == 0 && push.y == 0) {
        push.x = from.world_x() - to.world_x();
        push.y = from.world_z() - to.world_z();
    }
    if (push.x == 0 && push.y == 0) {
        push.y = -1;
    }
    SDL_Point primary{ (push.x > 0) ? 1 : (push.x < 0 ? -1 : 0),
                       (push.y > 0) ? 1 : (push.y < 0 ? -1 : 0) };
    std::vector<SDL_Point> directions;
    directions = {primary, {primary.x, 0}, {0, primary.y}, {primary.y, -primary.x}, {-primary.y, primary.x}};
    directions.erase(std::remove_if(directions.begin(), directions.end(), [](const SDL_Point& p){ return p.x == 0 && p.y == 0; }), directions.end());
    directions.erase(std::unique(directions.begin(), directions.end(), [](const SDL_Point& a, const SDL_Point& b){ return a.x == b.x && a.y == b.y; }), directions.end());
    if (directions.empty()) { directions = {{1,0},{-1,0},{0,1},{0,-1}}; }
    const auto inside_disallowed = [&](const world::GridPoint& bottom) {
        bool blocked = false;
        const Assets* assets = assets_owner_ ? assets_owner_ : (self_ ? self_->get_assets() : nullptr);
        if (!animation_update::detail::bottom_point_inside_playable_area(assets, bottom)) {
            return true;
        }
        visit_impassable_neighbors(*self_, [&](const Assets::FrameCollisionEntry*,
                                               const Asset* neighbor,
                                               const Area& area,
                                               const world::GridPoint&) {
            if (!neighbor || neighbor == self_ || !neighbor->info) return false;
            if (!area.contains_point(bottom.to_sdl_point())) return false;
            const auto it = std::find(blocking_neighbors.begin(), blocking_neighbors.end(), neighbor);
            if (it == blocking_neighbors.end()) { blocked = true; return true; }
            return false;
        });
        return blocked;
};
    const auto inside_any = [&](const world::GridPoint& bottom) {
        const Assets* assets = assets_owner_ ? assets_owner_ : (self_ ? self_->get_assets() : nullptr);
        if (!animation_update::detail::bottom_point_inside_playable_area(assets, bottom)) {
            return false;
        }
        bool inside = false;
        visit_impassable_neighbors(*self_, [&](const Assets::FrameCollisionEntry*,
                                               const Asset* neighbor,
                                               const Area& area,
                                               const world::GridPoint&) {
            if (!neighbor || neighbor == self_ || !neighbor->info) return false;
            if (area.contains_point(bottom.to_sdl_point())) { inside = true; return true; }
            return false;
        });
        return inside;
};
    const int max_steps = 12;
    for (SDL_Point dir : directions) {
        world::GridPoint candidate = world::grid_math::from_sdl(self_->world_xz_point(), self_->world_y(), self_->grid_resolution);
        bool      moved     = false;
        for (int step = 0; step < max_steps; ++step) {
            world::GridPoint next = world::grid_math::offset(candidate, dir);
            if (next.world_x() == candidate.world_x() && next.world_z() == candidate.world_z()) continue;
            world::GridPoint bottom_next = animation_update::detail::bottom_middle_for(*self_, next);
            if (inside_disallowed(bottom_next)) break;
            candidate = std::move(next);
            moved = true;
            if (!inside_any(bottom_next)) {
                break;
            }
        }
        if (moved) {
            self_->move_to_world_position(candidate.world_x(), self_->world_y(), candidate.world_z());
            return true;
        }
    }
    return false;
}

bool AnimationRuntime::attempt_unstick(SDL_Point from,
                                       SDL_Point to,
                                       const std::vector<const Asset*>& blockers) {
    const int world_y = self_ ? self_->world_y() : 0;
    const int layer = self_ ? self_->grid_resolution : 0;
    return attempt_unstick(world::grid_math::from_sdl(from, world_y, layer),
                           world::grid_math::from_sdl(to, world_y, layer),
                           blockers);
}

void AnimationRuntime::mark_progress_toward_checkpoints() {
    if (!self_ || !self_->info || !planner_iface_) {
        return;
    }
    if (planner_iface_->active_plan_mode_ != AnimationUpdate::ActivePlanMode::Plan2D) {
        return;
    }
    const int visited_thresh = planner_iface_->visited_thresh_;
    const int visited_sq     = visited_thresh * visited_thresh;
    while (next_checkpoint_index_ < planner_iface_->plan_.sanitized_checkpoints.size()) {
        const SDL_Point target_sdl  = planner_iface_->plan_.sanitized_checkpoints[next_checkpoint_index_];
        const int world_y = self_->world_y();
        const int layer = self_->grid_resolution;
        const world::GridPoint current = world::grid_math::from_sdl(self_->world_xz_point(), world_y, layer);
        const world::GridPoint target  = world::grid_math::from_sdl(target_sdl, world_y, layer);
        const int       dist_sq = animation_update::detail::distance_sq(current, target);
        bool reached = false;
        if (visited_thresh == 0) {
            reached = (self_->world_x() == target.world_x()) && (self_->world_z() == target.world_z());
        } else {
            reached = dist_sq <= visited_sq;
        }
        if (!reached) {
            break;
        }
        ++next_checkpoint_index_;
    }
}

void AnimationRuntime::mark_progress_toward_checkpoints_3d() {
    if (!self_ || !self_->info || !planner_iface_) {
        return;
    }
    if (planner_iface_->active_plan_mode_ != AnimationUpdate::ActivePlanMode::Plan3D) {
        return;
    }

    const int visited_thresh = planner_iface_->visited_thresh_;
    const long long visited_sq = static_cast<long long>(visited_thresh) * visited_thresh;
    while (next_checkpoint_index_ < planner_iface_->plan3d_.sanitized_checkpoints.size()) {
        const axis::WorldPos target = planner_iface_->plan3d_.sanitized_checkpoints[next_checkpoint_index_];
        const axis::WorldPos current{ self_->world_x(), self_->world_y(), self_->world_z() };
        const long long dist_sq = animation_update::detail::distance_sq_3d(current, target);

        bool reached = false;
        if (visited_thresh == 0) {
            reached = current.x == target.x && current.y == target.y && current.z == target.z;
        } else {
            reached = dist_sq <= visited_sq;
        }

        if (!reached) {
            break;
        }
        ++next_checkpoint_index_;
    }
}

bool AnimationRuntime::adjust_next_checkpoint(const std::vector<const Asset*>& blockers) {
    if (!self_ || !self_->info || !planner_iface_) {
        return false;
    }
    if (planner_iface_->active_plan_mode_ != AnimationUpdate::ActivePlanMode::Plan2D) {
        return false;
    }
    mark_progress_toward_checkpoints();
    const int world_y = self_->world_y();
    const int layer = self_->grid_resolution;
    const SDL_Point target_sdl = (next_checkpoint_index_ < planner_iface_->plan_.sanitized_checkpoints.size()) ? planner_iface_->plan_.sanitized_checkpoints[next_checkpoint_index_] : planner_iface_->final_dest;
    world::GridPoint target = world::grid_math::from_sdl(target_sdl, world_y, layer);
    world::GridPoint bottom_target = animation_update::detail::bottom_middle_for(*self_, target);
    const world::GridPoint start_point = world::grid_math::from_sdl(self_->world_xz_point(), world_y, layer);
    const auto block_context = active_path_blocking_context();
    SDL_Point push{0, 0};
    std::vector<const Asset*> influencing_neighbors;
    auto consider_neighbor = [&](const Asset* neighbor,
                                 const Area& area,
                                 const world::GridPoint& neighbor_bottom) {
        if (!neighbor || neighbor == self_ || !neighbor->info) return;
        bool relevant = area.contains_point(bottom_target.to_sdl_point()) ||
                        animation_update::detail::segment_hits_area(start_point, target, area);
        if (!relevant) {
            const int overlap_distance_sq =
                animation_update::detail::overlap_distance_sq_for_pair(*self_, *neighbor, block_context);
            relevant = overlap_distance_sq > 0 &&
                       animation_update::detail::distance_sq(bottom_target, neighbor_bottom) < overlap_distance_sq;
        }
        if (!relevant) return;
        SDL_Point center = area.get_center();
        push.x += bottom_target.world_x() - center.x;
        push.y += bottom_target.world_z() - center.y;
        influencing_neighbors.push_back(neighbor);
};
    if (!blockers.empty()) {
        std::unordered_set<const Asset*> blocker_lookup(blockers.begin(), blockers.end());
        visit_impassable_neighbors(*self_, [&](const Assets::FrameCollisionEntry*,
                                               const Asset* neighbor,
                                               const Area& area,
                                               const world::GridPoint& neighbor_bottom) {
            if (!neighbor || blocker_lookup.find(neighbor) == blocker_lookup.end()) {
                return false;
            }
            consider_neighbor(neighbor, area, neighbor_bottom);
            return false;
        });
    }
    if (influencing_neighbors.empty()) {
        visit_impassable_neighbors(*self_, [&](const Assets::FrameCollisionEntry*,
                                               const Asset* neighbor,
                                               const Area& area,
                                               const world::GridPoint& neighbor_bottom) {
            consider_neighbor(neighbor, area, neighbor_bottom);
            return false;
        });
    }
    if (push.x == 0 && push.y == 0) {
        push.x = target.world_x() - self_->world_x();
        push.y = target.world_z() - self_->world_z();
    }
    if (push.x == 0 && push.y == 0) {
        push.y = -1;
    }
    SDL_Point primary{ (push.x > 0) ? 1 : (push.x < 0 ? -1 : 0),
                       (push.y > 0) ? 1 : (push.y < 0 ? -1 : 0) };
    std::vector<SDL_Point> directions;
    directions = {primary, {primary.x, 0}, {0, primary.y}, {primary.y, -primary.x}, {-primary.y, primary.x}};
    directions.erase(std::remove_if(directions.begin(), directions.end(), [](const SDL_Point& p){ return p.x == 0 && p.y == 0; }), directions.end());
    directions.erase(std::unique(directions.begin(), directions.end(), [](const SDL_Point& a, const SDL_Point& b){ return a.x == b.x && a.y == b.y; }), directions.end());
    if (directions.empty()) { directions = {{1,0},{-1,0},{0,1},{0,-1}}; }
    const std::uint64_t retry_seed =
        path_variance_seed_for_retry(*self_, next_checkpoint_index_, movement_state_.replan_attempt_counter, 0x21ULL);
    reorder_directions_for_retry(directions, retry_seed);
    std::vector<SDL_Point> tail;
    for (std::size_t i = next_checkpoint_index_ + 1; i < planner_iface_->plan_.sanitized_checkpoints.size(); ++i) {
        tail.push_back(planner_iface_->plan_.sanitized_checkpoints[i]);
    }
    if (tail.empty() || !same_point(tail.back(), planner_iface_->final_dest)) {
        tail.push_back(planner_iface_->final_dest);
    }
    bool budget_exhausted = false;
    auto try_plan_with_targets = [&](const std::vector<SDL_Point>& targets) {
        if (targets.empty()) return false;
        if (!consume_replan_attempt_budget()) {
            budget_exhausted = true;
            return false;
        }
        CollisionQueryContext collision_context;
        collision_context.engagement_target_asset_id = planner_iface_->plan_.engagement_target_asset_id;
        collision_context.required_animation_tags = planner_iface_->plan_.movement_tag_filter.required_tags;
        collision_context.excluded_animation_tags = planner_iface_->plan_.movement_tag_filter.excluded_tags;
        collision_context.path_variance_seed =
            path_variance_seed_for_retry(*self_,
                                         next_checkpoint_index_,
                                         movement_state_.replan_attempt_counter,
                                         static_cast<std::uint64_t>(targets.size()));
        auto sanitized = sanitizer_.sanitize(*self_, targets, planner_iface_->visited_thresh_, &collision_context);
        if (sanitized.empty()) return false;
        Plan new_plan = planner_(*self_, sanitized, planner_iface_->visited_thresh_, grid(), &collision_context);
        new_plan.override_non_locked = planner_iface_->plan_.override_non_locked;
        new_plan.engagement_target_asset_id = planner_iface_->plan_.engagement_target_asset_id;
        new_plan.attacking_enabled = planner_iface_->plan_.attacking_enabled;
        new_plan.movement_tag_filter = planner_iface_->plan_.movement_tag_filter;
        if (new_plan.strides.empty()) return false;
        planner_iface_->plan_ = std::move(new_plan);
        planner_iface_->final_dest = planner_iface_->plan_.final_dest;
        stride_index_ = 0;
        stride_frame_counter_ = 0;
        next_checkpoint_index_ = 0;
        mark_progress_toward_checkpoints();
        return true;
};
    const int max_steps = 24;
    for (SDL_Point dir : directions) {
        world::GridPoint candidate = target;
        for (int step = 0; step < max_steps; ++step) {
            world::GridPoint next = world::grid_math::offset(candidate, dir);
            if (next.world_x() == candidate.world_x() && next.world_z() == candidate.world_z()) continue;
            world::GridPoint bottom_next = animation_update::detail::bottom_middle_for(*self_, next);
            if (point_in_impassable(bottom_next, self_)) break;
            candidate = std::move(next);
            std::vector<SDL_Point> attempt_targets;
            attempt_targets.push_back(candidate.to_sdl_point());
            auto it_begin = tail.begin();
            if (!tail.empty() && same_point(tail.front(), candidate.to_sdl_point())) {
                ++it_begin;
            }
            attempt_targets.insert(attempt_targets.end(), it_begin, tail.end());
            if (try_plan_with_targets(attempt_targets)) {
                return true;
            }
            if (budget_exhausted) {
                return false;
            }
        }
    }
    return false;
}

bool AnimationRuntime::adjust_next_checkpoint_3d(const std::vector<const Asset*>& blockers) {
    if (!self_ || !self_->info || !planner_iface_) {
        return false;
    }
    if (planner_iface_->active_plan_mode_ != AnimationUpdate::ActivePlanMode::Plan3D) {
        return false;
    }

    auto same_world_pos = [](const axis::WorldPos& lhs, const axis::WorldPos& rhs) {
        return lhs.x == rhs.x && lhs.y == rhs.y && lhs.z == rhs.z;
    };

    mark_progress_toward_checkpoints_3d();
    const axis::WorldPos target_pos =
        (next_checkpoint_index_ < planner_iface_->plan3d_.sanitized_checkpoints.size())
            ? planner_iface_->plan3d_.sanitized_checkpoints[next_checkpoint_index_]
            : planner_iface_->final_dest_3d;
    world::GridPoint target =
        world::GridPoint::make_virtual(target_pos.x, target_pos.y, target_pos.z, self_->grid_resolution);
    world::GridPoint bottom_target = animation_update::detail::bottom_middle_for(*self_, target);
    const world::GridPoint start_point =
        world::GridPoint::make_virtual(self_->world_x(), self_->world_y(), self_->world_z(), self_->grid_resolution);
    const auto block_context = active_path_blocking_context();

    SDL_Point push{ 0, 0 };
    std::vector<const Asset*> influencing_neighbors;
    auto consider_neighbor = [&](const Asset* neighbor,
                                 const Area& area,
                                 const world::GridPoint& neighbor_bottom) {
        if (!neighbor || neighbor == self_ || !neighbor->info) {
            return;
        }

        bool relevant = area.contains_point(bottom_target.to_sdl_point()) ||
                        animation_update::detail::segment_hits_area(start_point, target, area);
        if (!relevant) {
            const int overlap_distance_sq =
                animation_update::detail::overlap_distance_sq_for_pair(*self_, *neighbor, block_context);
            relevant = overlap_distance_sq > 0 &&
                       animation_update::detail::distance_sq(bottom_target, neighbor_bottom) < overlap_distance_sq;
        }
        if (!relevant) {
            return;
        }
        const SDL_Point center = area.get_center();
        push.x += bottom_target.world_x() - center.x;
        push.y += bottom_target.world_z() - center.y;
        influencing_neighbors.push_back(neighbor);
    };

    if (!blockers.empty()) {
        std::unordered_set<const Asset*> blocker_lookup(blockers.begin(), blockers.end());
        visit_impassable_neighbors(*self_, [&](const Assets::FrameCollisionEntry*,
                                               const Asset* neighbor,
                                               const Area& area,
                                               const world::GridPoint& neighbor_bottom) {
            if (!neighbor || blocker_lookup.find(neighbor) == blocker_lookup.end()) {
                return false;
            }
            consider_neighbor(neighbor, area, neighbor_bottom);
            return false;
        });
    }
    if (influencing_neighbors.empty()) {
        visit_impassable_neighbors(*self_, [&](const Assets::FrameCollisionEntry*,
                                               const Asset* neighbor,
                                               const Area& area,
                                               const world::GridPoint& neighbor_bottom) {
            consider_neighbor(neighbor, area, neighbor_bottom);
            return false;
        });
    }

    if (push.x == 0 && push.y == 0) {
        push.x = target.world_x() - self_->world_x();
        push.y = target.world_z() - self_->world_z();
    }
    if (push.x == 0 && push.y == 0) {
        push.y = -1;
    }

    SDL_Point primary{ (push.x > 0) ? 1 : (push.x < 0 ? -1 : 0),
                       (push.y > 0) ? 1 : (push.y < 0 ? -1 : 0) };
    std::vector<SDL_Point> directions;
    directions = {primary, {primary.x, 0}, {0, primary.y}, {primary.y, -primary.x}, {-primary.y, primary.x}};
    directions.erase(std::remove_if(directions.begin(), directions.end(), [](const SDL_Point& p){ return p.x == 0 && p.y == 0; }), directions.end());
    directions.erase(std::unique(directions.begin(), directions.end(), [](const SDL_Point& a, const SDL_Point& b){ return a.x == b.x && a.y == b.y; }), directions.end());
    if (directions.empty()) { directions = {{1,0},{-1,0},{0,1},{0,-1}}; }
    const std::uint64_t retry_seed_3d =
        path_variance_seed_for_retry(*self_, next_checkpoint_index_, movement_state_.replan_attempt_counter, 0x31ULL);
    reorder_directions_for_retry(directions, retry_seed_3d);

    std::vector<axis::WorldPos> tail;
    for (std::size_t i = next_checkpoint_index_ + 1; i < planner_iface_->plan3d_.sanitized_checkpoints.size(); ++i) {
        tail.push_back(planner_iface_->plan3d_.sanitized_checkpoints[i]);
    }
    if (tail.empty() || !same_world_pos(tail.back(), planner_iface_->final_dest_3d)) {
        tail.push_back(planner_iface_->final_dest_3d);
    }

    bool budget_exhausted = false;
    auto try_plan_with_targets = [&](const std::vector<axis::WorldPos>& targets) {
        if (targets.empty()) {
            return false;
        }
        if (!consume_replan_attempt_budget()) {
            budget_exhausted = true;
            return false;
        }
        auto sanitized = sanitizer_3d_.sanitize(*self_, targets, planner_iface_->visited_thresh_);
        if (sanitized.empty()) {
            return false;
        }

        CollisionQueryContext collision_context;
        collision_context.engagement_target_asset_id = planner_iface_->plan3d_.engagement_target_asset_id;
        collision_context.required_animation_tags = planner_iface_->plan3d_.movement_tag_filter.required_tags;
        collision_context.excluded_animation_tags = planner_iface_->plan3d_.movement_tag_filter.excluded_tags;
        collision_context.path_variance_seed =
            path_variance_seed_for_retry(*self_,
                                         next_checkpoint_index_,
                                         movement_state_.replan_attempt_counter,
                                         static_cast<std::uint64_t>(targets.size()));
        Plan3D new_plan = planner_3d_(*self_, sanitized, planner_iface_->visited_thresh_, grid(), &collision_context);
        new_plan.override_non_locked = planner_iface_->plan3d_.override_non_locked;
        new_plan.engagement_target_asset_id = planner_iface_->plan3d_.engagement_target_asset_id;
        new_plan.attacking_enabled = planner_iface_->plan3d_.attacking_enabled;
        new_plan.movement_tag_filter = planner_iface_->plan3d_.movement_tag_filter;
        if (new_plan.strides.empty()) {
            return false;
        }

        planner_iface_->plan3d_ = std::move(new_plan);
        planner_iface_->final_dest_3d = planner_iface_->plan3d_.final_dest;
        planner_iface_->active_plan_mode_ = AnimationUpdate::ActivePlanMode::Plan3D;
        stride_index_ = 0;
        stride_frame_counter_ = 0;
        next_checkpoint_index_ = 0;
        mark_progress_toward_checkpoints_3d();
        return true;
    };

    const int max_steps = 24;
    for (const SDL_Point dir : directions) {
        world::GridPoint candidate = target;
        for (int step = 0; step < max_steps; ++step) {
            world::GridPoint next = world::grid_math::offset(candidate, dir);
            if (next.world_x() == candidate.world_x() && next.world_z() == candidate.world_z()) {
                continue;
            }
            world::GridPoint bottom_next = animation_update::detail::bottom_middle_for(*self_, next);
            if (point_in_impassable(bottom_next, self_)) {
                break;
            }
            candidate = std::move(next);

            std::vector<axis::WorldPos> attempt_targets;
            const axis::WorldPos candidate_pos{
                candidate.world_x(),
                candidate.world_y(),
                candidate.world_z()
            };
            attempt_targets.push_back(candidate_pos);

            auto it_begin = tail.begin();
            if (!tail.empty() && same_world_pos(tail.front(), candidate_pos)) {
                ++it_begin;
            }
            attempt_targets.insert(attempt_targets.end(), it_begin, tail.end());
            if (try_plan_with_targets(attempt_targets)) {
                return true;
            }
            if (budget_exhausted) {
                return false;
            }
        }
    }

    return false;
}

bool AnimationRuntime::handle_blocked_path(const world::GridPoint& from,
                                           const world::GridPoint& to,
                                           const std::vector<const Asset*>& blockers) {
    if (!planner_iface_) {
        return false;
    }

    const bool auto_plan_active =
        planner_iface_->active_plan_mode_ == AnimationUpdate::ActivePlanMode::Plan2D ||
        planner_iface_->active_plan_mode_ == AnimationUpdate::ActivePlanMode::Plan3D;
    bool moved = false;
    if (!auto_plan_active) {
        moved = attempt_unstick(from, to, blockers);
    }
    if (planner_iface_->active_plan_mode_ == AnimationUpdate::ActivePlanMode::Plan3D) {
        if (moved) {
            mark_progress_toward_checkpoints_3d();
        }
        if (adjust_next_checkpoint_3d(blockers)) {
            return true;
        }
        if (replan_to_destination_3d()) {
            return true;
        }
        return moved;
    }

    if (moved) {
        mark_progress_toward_checkpoints();
    }
    if (adjust_next_checkpoint(blockers)) {
        return true;
    }
    if (replan_to_destination()) {
        return true;
    }

    return moved;
}

bool AnimationRuntime::handle_blocked_path(SDL_Point from,
                                           SDL_Point to,
                                           const std::vector<const Asset*>& blockers) {
    const int world_y = self_ ? self_->world_y() : 0;
    const int layer = self_ ? self_->grid_resolution : 0;
    return handle_blocked_path(world::grid_math::from_sdl(from, world_y, layer),
                               world::grid_math::from_sdl(to, world_y, layer),
                               blockers);
}

bool AnimationRuntime::replan_to_destination() {
    if (!self_ || !self_->info || !planner_iface_) {
        return false;
    }
    if (planner_iface_->active_plan_mode_ != AnimationUpdate::ActivePlanMode::Plan2D) {
        return false;
    }
    const int visited_sq = planner_iface_->visited_thresh_ * planner_iface_->visited_thresh_;
    if (visited_sq > 0 && animation_update::detail::distance_sq(self_->world_xz_point(), planner_iface_->final_dest) <= visited_sq) {
        return false;
    }
    mark_progress_toward_checkpoints();
    std::vector<SDL_Point> checkpoints;
    for (std::size_t i = next_checkpoint_index_; i < planner_iface_->plan_.sanitized_checkpoints.size(); ++i) {
        checkpoints.push_back(planner_iface_->plan_.sanitized_checkpoints[i]);
    }
    if (checkpoints.empty() || !same_point(checkpoints.back(), planner_iface_->final_dest)) {
        checkpoints.push_back(planner_iface_->final_dest);
    }
    if (!consume_replan_attempt_budget()) {
        return false;
    }
    CollisionQueryContext collision_context;
    collision_context.engagement_target_asset_id = planner_iface_->plan_.engagement_target_asset_id;
    collision_context.required_animation_tags = planner_iface_->plan_.movement_tag_filter.required_tags;
    collision_context.excluded_animation_tags = planner_iface_->plan_.movement_tag_filter.excluded_tags;
    collision_context.path_variance_seed =
        path_variance_seed_for_retry(*self_,
                                     next_checkpoint_index_,
                                     movement_state_.replan_attempt_counter,
                                     0x41ULL);
    auto sanitized = sanitizer_.sanitize(*self_, checkpoints, planner_iface_->visited_thresh_, &collision_context);
    if (sanitized.empty()) {
        return false;
    }
    Plan new_plan = planner_(*self_, sanitized, planner_iface_->visited_thresh_, grid(), &collision_context);
    new_plan.override_non_locked = planner_iface_->plan_.override_non_locked;
    new_plan.engagement_target_asset_id = planner_iface_->plan_.engagement_target_asset_id;
    new_plan.attacking_enabled = planner_iface_->plan_.attacking_enabled;
    new_plan.movement_tag_filter = planner_iface_->plan_.movement_tag_filter;
    if (new_plan.strides.empty()) {
        return false;
    }
    planner_iface_->plan_ = std::move(new_plan);
    planner_iface_->final_dest = planner_iface_->plan_.final_dest;
    stride_index_ = 0;
    stride_frame_counter_ = 0;
    next_checkpoint_index_ = 0;
    mark_progress_toward_checkpoints();
    return true;
}

bool AnimationRuntime::replan_to_destination_3d() {
    if (!self_ || !self_->info || !planner_iface_) {
        return false;
    }
    if (planner_iface_->active_plan_mode_ != AnimationUpdate::ActivePlanMode::Plan3D) {
        return false;
    }

    auto same_world_pos = [](const axis::WorldPos& lhs, const axis::WorldPos& rhs) {
        return lhs.x == rhs.x && lhs.y == rhs.y && lhs.z == rhs.z;
    };

    const long long visited_sq =
        static_cast<long long>(planner_iface_->visited_thresh_) * planner_iface_->visited_thresh_;
    const axis::WorldPos current{ self_->world_x(), self_->world_y(), self_->world_z() };
    if (visited_sq > 0 &&
        animation_update::detail::distance_sq_3d(current, planner_iface_->final_dest_3d) <= visited_sq) {
        return false;
    }

    mark_progress_toward_checkpoints_3d();
    std::vector<axis::WorldPos> checkpoints;
    for (std::size_t i = next_checkpoint_index_; i < planner_iface_->plan3d_.sanitized_checkpoints.size(); ++i) {
        checkpoints.push_back(planner_iface_->plan3d_.sanitized_checkpoints[i]);
    }
    if (checkpoints.empty() || !same_world_pos(checkpoints.back(), planner_iface_->final_dest_3d)) {
        checkpoints.push_back(planner_iface_->final_dest_3d);
    }

    if (!consume_replan_attempt_budget()) {
        return false;
    }
    CollisionQueryContext collision_context;
    collision_context.engagement_target_asset_id = planner_iface_->plan3d_.engagement_target_asset_id;
    collision_context.required_animation_tags = planner_iface_->plan3d_.movement_tag_filter.required_tags;
    collision_context.excluded_animation_tags = planner_iface_->plan3d_.movement_tag_filter.excluded_tags;
    collision_context.path_variance_seed =
        path_variance_seed_for_retry(*self_,
                                     next_checkpoint_index_,
                                     movement_state_.replan_attempt_counter,
                                     0x51ULL);
    auto sanitized = sanitizer_3d_.sanitize(*self_, checkpoints, planner_iface_->visited_thresh_);
    if (sanitized.empty()) {
        return false;
    }

    Plan3D new_plan = planner_3d_(*self_, sanitized, planner_iface_->visited_thresh_, grid(), &collision_context);
    new_plan.override_non_locked = planner_iface_->plan3d_.override_non_locked;
    new_plan.engagement_target_asset_id = planner_iface_->plan3d_.engagement_target_asset_id;
    new_plan.attacking_enabled = planner_iface_->plan3d_.attacking_enabled;
    new_plan.movement_tag_filter = planner_iface_->plan3d_.movement_tag_filter;
    if (new_plan.strides.empty()) {
        return false;
    }

    planner_iface_->plan3d_ = std::move(new_plan);
    planner_iface_->final_dest_3d = planner_iface_->plan3d_.final_dest;
    planner_iface_->active_plan_mode_ = AnimationUpdate::ActivePlanMode::Plan3D;
    stride_index_ = 0;
    stride_frame_counter_ = 0;
    next_checkpoint_index_ = 0;
    mark_progress_toward_checkpoints_3d();
    return true;
}

vibble::grid::Grid& AnimationRuntime::grid() const {
    if (grid_service_) return *grid_service_;
    return vibble::grid::global_grid();
}

int AnimationRuntime::effective_grid_resolution(std::optional<int> override_resolution) const {
    return resolve_effective_grid_resolution(self_, grid(), override_resolution);
}

SDL_Point AnimationRuntime::convert_delta_to_world(SDL_Point delta, int resolution) const {
    const int clamped_resolution = vibble::grid::clamp_resolution(resolution);
    (void)clamped_resolution;
    // Controller/movement deltas are authored in world-space pixels (X/Z).
    // Resolution affects planning checkpoints but not per-frame root-motion deltas.
    return delta;
}

bool AnimationRuntime::has_active_plan() const {
    if (!planner_iface_) {
        return false;
    }
    if (planner_iface_->active_plan_mode_ == AnimationUpdate::ActivePlanMode::Plan2D) {
        return !planner_iface_->plan_.strides.empty();
    }
    if (planner_iface_->active_plan_mode_ == AnimationUpdate::ActivePlanMode::Plan3D) {
        return !planner_iface_->plan3d_.strides.empty();
    }
    return false;
}
