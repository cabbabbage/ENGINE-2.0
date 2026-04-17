#include "animation_runtime.hpp"

#include <algorithm>
#include <cmath>
#include <string>
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
#include "utils/transform_smoothing.hpp"

namespace {
template <typename Fn>
bool visit_impassable_neighbors(const Asset& asset, Fn&& fn) {
    const Assets* assets = asset.get_assets();
    if (!assets) {
        return false;
    }

    std::vector<const Assets::FrameCollisionEntry*> entries;
    const int search_radius = (asset.info && asset.info->NeighborSearchRadius > 0)
        ? asset.info->NeighborSearchRadius
        : 0;
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

std::vector<SDL_Point> build_escape_directions(SDL_Point primary) {
    std::vector<SDL_Point> dirs;
    dirs.reserve(5);
    auto add = [&](SDL_Point d) {
        if (d.x == 0 && d.y == 0) return;
        for (const auto& e : dirs) { if (e.x == d.x && e.y == d.y) return; }
        dirs.push_back(d);
    };
    if (primary.x == 0 && primary.y == 0) {
        dirs = {{1,0},{-1,0},{0,1},{0,-1}};
    } else {
        add(primary);
        add({primary.x, 0});
        add({0, primary.y});
        add({primary.y, -primary.x});
        add({-primary.y, primary.x});
    }
    return dirs;
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
}

AnimationRuntime::AnimationRuntime(Asset* self, Assets* assets)
    : self_(self), assets_owner_(assets), grid_service_(&vibble::grid::global_grid()) {}

void AnimationRuntime::set_planner(AnimationUpdate* planner) {
    if (planner_iface_ && planner_iface_ != planner) {
        planner_iface_->set_runtime(nullptr);
    }
    planner_iface_ = planner;
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
    reverse_playback_mode_ = ReversePlaybackMode::None;
    reverse_playback_animation_id_.clear();
}

bool AnimationRuntime::reverse_mode_applies_to_current_animation() const {
    return reverse_playback_mode_ != ReversePlaybackMode::None &&
           !reverse_playback_animation_id_.empty() &&
           self_ &&
           self_->current_animation == reverse_playback_animation_id_;
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

    reverse_playback_mode_ = mode;
    reverse_playback_animation_id_ = self_->current_animation;
    lock_on_end_active_ = false;
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
                return;
            }
        }

        if (planner_iface_->has_pending_move()) {
            const auto& req = planner_iface_->pending_move_;
            if (!should_defer_for_non_locked(req.override_non_locked)) {
                apply_pending_move();
                return;
            }
        }
        if (planner_iface_->has_pending_move_3d()) {
            const auto& req3d = planner_iface_->pending_move_3d_;
            if (!should_defer_for_non_locked(req3d.override_non_locked)) {
                apply_pending_move_3d();
                return;
            }
        }
    }

    advance(self_->current_frame);
}

void AnimationRuntime::apply_pending_move() {
    if (!planner_iface_ || !self_) return;

    const auto req = planner_iface_->consume_move_request();
    const int  resolution = effective_grid_resolution(std::nullopt);
    const SDL_Point from{ self_->world_x(), self_->world_z() };
    SDL_Point world_delta = convert_delta_to_world(req.delta, resolution);
    const SDL_Point to{ from.x + world_delta.x, from.y + world_delta.y };

    SDL_Point final_position = from;
    if (world_delta.x != 0 || world_delta.y != 0) {
        if (!path_blocked(from, to, self_, nullptr)) {
            final_position = to;
        } else {
            const int steps = std::max(std::abs(world_delta.x), std::abs(world_delta.y));
            if (steps > 0) {
                const double step_x = static_cast<double>(world_delta.x) / static_cast<double>(steps);
                const double step_y = static_cast<double>(world_delta.y) / static_cast<double>(steps);
                double       accum_x = static_cast<double>(from.x);
                double       accum_y = static_cast<double>(from.y);
                SDL_Point    current = from;
                for (int i = 0; i < steps; ++i) {
                    accum_x += step_x;
                    accum_y += step_y;
                    SDL_Point candidate{ static_cast<int>(std::round(accum_x)), static_cast<int>(std::round(accum_y)) };
                    if (candidate.x == current.x && candidate.y == current.y) continue;
                    if (path_blocked(current, candidate, self_, nullptr)) break;
                    final_position = candidate;
                    current        = candidate;
                }
            }
        }
    }

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
        switch_to(resolved, path_index_for(resolved));
    } else {
        if (!advance(self_->current_frame)) {
            if (self_->dead) {
                return;
            }
            switch_to(resolved, path_index_for(resolved));
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
    if (world_delta.x != 0 || world_delta.y != 0 || world_delta.z != 0) {
        const world::GridPoint gp_from = world::GridPoint::make_virtual(from.x, from.y, from.z, self_->grid_resolution);
        const world::GridPoint gp_to = world::GridPoint::make_virtual(to.x, to.y, to.z, self_->grid_resolution);
        if (!path_blocked(gp_from, gp_to, self_, nullptr)) {
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
                    if (path_blocked(current_gp, candidate_gp, self_, nullptr)) {
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
        switch_to(resolved, path_index_for(resolved));
    } else {
        if (!advance(self_->current_frame)) {
            if (self_->dead) {
                return;
            }
            switch_to(resolved, path_index_for(resolved));
        }
    }
}

bool AnimationRuntime::advance(AnimationFrame*& frame) {
    if (!self_ || !self_->info) {
        return false;
    }

    auto it = self_->info->animations.find(self_->current_animation);
    if (it == self_->info->animations.end()) {
        return false;
    }

    Animation* anim = &it->second;
    std::size_t path_index = path_index_for(self_->current_animation);
    if (!reverse_mode_applies_to_current_animation()) {
        clear_reverse_playback_state();
    }
    if (!frame) {
        frame = anim->get_first_frame(path_index);
        if (!frame) {
            return false;
        }
    }

    const bool is_player = self_->info && self_->info->type == asset_types::player;
    const bool reverse_command_active = reverse_mode_applies_to_current_animation();
    const bool static_blocked = self_->static_frame && !reverse_command_active;
    const bool locked_blocked = anim->locked && !reverse_command_active;
    bool should_skip = !is_player && (static_blocked || locked_blocked || anim->is_frozen() || lock_on_end_active_);
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
        self_->static_frame = self_->static_frame || anim->is_frozen() || anim->locked || lock_on_end_active_;
        return true;
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
    bool advanced_any = false;
    while (self_->frame_progress >= frame_interval) {
        self_->frame_progress -= frame_interval;

        if (reverse_mode_applies_to_current_animation()) {
            if (frame->prev) {
                frame = frame->prev;
                advanced_any = true;
                continue;
            }

            if (reverse_playback_mode_ == ReversePlaybackMode::ReverseUntilStopCurrentAnimation) {
                frame = last_frame_for(*anim, path_index);
                if (!frame) {
                    return false;
                }
                advanced_any = true;
                continue;
            }

            const ReversePlaybackMode mode_at_boundary = reverse_playback_mode_;
            clear_reverse_playback_state();
            if (mode_at_boundary == ReversePlaybackMode::ReverseToDefaultAtStart) {
                switch_to(animation_update::detail::kDefaultAnimation,
                          path_index_for(animation_update::detail::kDefaultAnimation));
                frame = self_->current_frame;
                if (!frame) {
                    return false;
                }
                it = self_->info->animations.find(self_->current_animation);
                if (it == self_->info->animations.end()) {
                    return false;
                }
                anim = &it->second;
                path_index = path_index_for(self_->current_animation);
                advanced_any = true;
                continue;
            }

            continue;
        }

        if (frame->next) {
            frame = frame->next;
            advanced_any = true;
            continue;
        }

        const Animation::OnEndDirective directive = anim->on_end_behavior;
        switch (directive) {
        case Animation::OnEndDirective::Loop: {
            frame = anim->get_first_frame(path_index);
            if (!frame) {
                return false;
            }
            advanced_any = true;
            break;
        }
        case Animation::OnEndDirective::Kill:
            self_->Delete();
            return false;
        case Animation::OnEndDirective::Lock:
            lock_on_end_active_ = true;
            self_->static_frame = true;
            return true;
        case Animation::OnEndDirective::Reverse:
            activate_reverse_playback(ReversePlaybackMode::ReverseToDefaultAtStart);
            lock_on_end_active_ = false;
            self_->static_frame = false;
            break;
        case Animation::OnEndDirective::Animation: {
            const std::string requested = anim->on_end_animation.empty()
                                              ? std::string{animation_update::detail::kDefaultAnimation}
                                              : anim->on_end_animation;
            const std::string resolved = resolve_animation(*self_, requested);
            switch_to(resolved, path_index_for(requested));
            frame = self_->current_frame;
            if (!frame) {
                return false;
            }
            it = self_->info->animations.find(self_->current_animation);
            if (it == self_->info->animations.end()) {
                return false;
            }
            anim = &it->second;
            path_index = path_index_for(self_->current_animation);
            advanced_any = true;
            break;
        }
        case Animation::OnEndDirective::Default:
        default:
            switch_to(animation_update::detail::kDefaultAnimation,
                      path_index_for(animation_update::detail::kDefaultAnimation));
            frame = self_->current_frame;
            if (!frame) {
                return false;
            }
            it = self_->info->animations.find(self_->current_animation);
            if (it == self_->info->animations.end()) {
                return false;
            }
            anim = &it->second;
            path_index = path_index_for(self_->current_animation);
            advanced_any = true;
            break;
        }
    }
    if (advanced_any) {
        self_->mark_composite_dirty();
        self_->mark_anchors_dirty();
    }
    return true;
}

void AnimationRuntime::switch_to(const std::string& anim_id, std::size_t path_index) {
    if (!self_ || !self_->info) {
        return;
    }

    clear_reverse_playback_state();
    lock_on_end_active_ = false;

    auto it = self_->info->animations.find(anim_id);
    if (it == self_->info->animations.end()) {
        auto def = self_->info->animations.find(animation_update::detail::kDefaultAnimation);
        if (def == self_->info->animations.end()) {
            if (self_->info->animations.empty()) {
                return;
            }
            it = self_->info->animations.begin();
        } else {
            it = def;
        }
    }

    Animation& anim = it->second;
    path_index = anim.clamp_path_index(path_index);
    AnimationFrame* new_frame = anim.get_first_frame(path_index);
    self_->current_animation = it->first;
    self_->current_frame     = new_frame;
    {
        const bool is_player = self_->info && self_->info->type == asset_types::player;
        self_->static_frame  = is_player ? false : (anim.is_frozen() || anim.locked);
    }
    self_->frame_progress    = 0.0f;
    active_paths_[self_->current_animation] = path_index;
    self_->mark_composite_dirty();
    self_->mark_anchors_dirty();
}

bool AnimationRuntime::should_defer_for_non_locked(bool override_non_locked) const {
    if (override_non_locked || !self_ || !self_->info) {
        return false;
    }

    auto it = self_->info->animations.find(self_->current_animation);
    if (it == self_->info->animations.end()) {
        return false;
    }

    if (self_->current_animation == animation_update::detail::kDefaultAnimation) {
        return false;
    }

    const Animation& anim = it->second;
    return !anim.locked;
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
                                    std::vector<const Asset*>* blockers) const {
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
    visit_impassable_neighbors(*self_, [&](const Assets::FrameCollisionEntry* entry,
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
        const bool touches_segment = animation_update::detail::segment_hits_area(from, to, area);
        bool overlaps = false;
        if (!contains_from && !contains_to && !touches_segment) {
            const bool overlap_check = animation_update::detail::should_consider_overlap(*self_, *neighbor);
            if (overlap_check) {
                overlaps = animation_update::detail::distance_sq(dest_bottom, neighbor_bottom) <
                           animation_update::detail::kOverlapDistanceSq;
            }
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
                                    std::vector<const Asset*>* blockers) const {
    const int world_y = self_ ? self_->world_y() : 0;
    const int layer = self_ ? self_->grid_resolution : 0;
    const world::GridPoint gp_from = world::grid_math::from_sdl(from, world_y, layer);
    const world::GridPoint gp_to   = world::grid_math::from_sdl(to, world_y, layer);
    return path_blocked(gp_from, gp_to, ignored, blockers);
}

bool AnimationRuntime::attempt_unstick(const world::GridPoint& from,
                                       const world::GridPoint& to,
                                       const std::vector<const Asset*>& blockers) {
    if (!self_ || !self_->info) {
        return false;
    }
    world::GridPoint bottom_from = animation_update::detail::bottom_middle_for(*self_, from);
    world::GridPoint bottom_to   = animation_update::detail::bottom_middle_for(*self_, to);
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
                const bool overlap_check = animation_update::detail::should_consider_overlap(*self_, *neighbor);
                if (overlap_check) {
                    overlaps = animation_update::detail::distance_sq(bottom_from, neighbor_bottom) <
                               animation_update::detail::kOverlapDistanceSq;
                }
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
    std::vector<SDL_Point> directions = build_escape_directions(primary);
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
    SDL_Point push{0, 0};
    std::vector<const Asset*> influencing_neighbors;
    auto consider_neighbor = [&](const Asset* neighbor,
                                 const Area& area,
                                 const world::GridPoint& neighbor_bottom) {
        if (!neighbor || neighbor == self_ || !neighbor->info) return;
        bool relevant = area.contains_point(bottom_target.to_sdl_point()) ||
                        animation_update::detail::segment_hits_area(start_point, target, area);
        if (!relevant) {
            const bool overlap_check = animation_update::detail::should_consider_overlap(*self_, *neighbor);
            if (overlap_check) {
                relevant = animation_update::detail::distance_sq(bottom_target, neighbor_bottom) < animation_update::detail::kOverlapDistanceSq;
            }
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
    std::vector<SDL_Point> directions = build_escape_directions(primary);
    std::vector<SDL_Point> tail;
    for (std::size_t i = next_checkpoint_index_ + 1; i < planner_iface_->plan_.sanitized_checkpoints.size(); ++i) {
        tail.push_back(planner_iface_->plan_.sanitized_checkpoints[i]);
    }
    if (tail.empty() || !same_point(tail.back(), planner_iface_->final_dest)) {
        tail.push_back(planner_iface_->final_dest);
    }
    auto try_plan_with_targets = [&](const std::vector<SDL_Point>& targets) {
        if (targets.empty()) return false;
        auto sanitized = sanitizer_.sanitize(*self_, targets, planner_iface_->visited_thresh_);
        if (sanitized.empty()) return false;
        Plan new_plan = planner_(*self_, sanitized, planner_iface_->visited_thresh_, grid());
        new_plan.override_non_locked = planner_iface_->plan_.override_non_locked;
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
            const bool overlap_check = animation_update::detail::should_consider_overlap(*self_, *neighbor);
            if (overlap_check) {
                relevant = animation_update::detail::distance_sq(bottom_target, neighbor_bottom) <
                           animation_update::detail::kOverlapDistanceSq;
            }
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
    std::vector<SDL_Point> directions = build_escape_directions(primary);

    std::vector<axis::WorldPos> tail;
    for (std::size_t i = next_checkpoint_index_ + 1; i < planner_iface_->plan3d_.sanitized_checkpoints.size(); ++i) {
        tail.push_back(planner_iface_->plan3d_.sanitized_checkpoints[i]);
    }
    if (tail.empty() || !same_world_pos(tail.back(), planner_iface_->final_dest_3d)) {
        tail.push_back(planner_iface_->final_dest_3d);
    }

    auto try_plan_with_targets = [&](const std::vector<axis::WorldPos>& targets) {
        if (targets.empty()) {
            return false;
        }
        auto sanitized = sanitizer_3d_.sanitize(*self_, targets, planner_iface_->visited_thresh_);
        if (sanitized.empty()) {
            return false;
        }

        Plan3D new_plan = planner_3d_(*self_, sanitized, planner_iface_->visited_thresh_, grid());
        new_plan.override_non_locked = planner_iface_->plan3d_.override_non_locked;
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

    bool moved = attempt_unstick(from, to, blockers);
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
    auto sanitized = sanitizer_.sanitize(*self_, checkpoints, planner_iface_->visited_thresh_);
    if (sanitized.empty()) {
        return false;
    }
    Plan new_plan = planner_(*self_, sanitized, planner_iface_->visited_thresh_, grid());
    new_plan.override_non_locked = planner_iface_->plan_.override_non_locked;
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

    auto sanitized = sanitizer_3d_.sanitize(*self_, checkpoints, planner_iface_->visited_thresh_);
    if (sanitized.empty()) {
        return false;
    }

    Plan3D new_plan = planner_3d_(*self_, sanitized, planner_iface_->visited_thresh_, grid());
    new_plan.override_non_locked = planner_iface_->plan3d_.override_non_locked;
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
