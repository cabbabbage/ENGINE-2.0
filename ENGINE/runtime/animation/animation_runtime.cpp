#include "animation_runtime.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>
#include <unordered_map>

#include "assets/Asset.hpp"
#include "utils/log.hpp"
#include "assets/animation.hpp"
#include "assets/animation_frame.hpp"
#include "assets/asset_info.hpp"
#include "assets/asset_types.hpp"
#include "core/AssetsManager.hpp"
#include "core/asset_list.hpp"
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
    const AssetList* list = asset.get_impassable_naighbors();
    if (!list) {
        return false;
    }

    const auto visit_bucket = [&](const std::vector<Asset*>& bucket) {
        for (Asset* neighbor : bucket) {
            if (fn(neighbor)) {
                return true;
            }
        }
        return false;
};

    if (visit_bucket(list->top_unsorted())) {
        return true;
    }
    if (visit_bucket(list->middle_sorted())) {
        return true;
    }
    if (visit_bucket(list->bottom_unsorted())) {
        return true;
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
}

AnimationRuntime::AnimationRuntime(Asset* self, Assets* assets)
    : self_(self), assets_owner_(assets), grid_service_(&vibble::grid::global_grid()) {}

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

void AnimationRuntime::update() {
    if (!self_ || !self_->info || !planner_iface_) {
        return;
    }

    float dt = 1.0f / 60.0f;
    if (assets_owner_) {
        dt = assets_owner_->frame_delta_seconds();
    }
    if (!(dt > 0.0f)) {
        dt = 1.0f / 60.0f;
    }

    struct SuppressionDecay {
        AnimationRuntime* runtime = nullptr;
        ~SuppressionDecay() {
            if (runtime && runtime->suppress_root_motion_frames_ > 0) {
                --runtime->suppress_root_motion_frames_;
            }
        }
    } decay{ this };

    const bool got_input = planner_iface_->consume_input_event();

    const bool has_plan = !planner_iface_->plan_.strides.empty();
    const bool plan_deferred = has_plan &&
                               should_defer_for_non_locked(planner_iface_->plan_.override_non_locked);

    if (has_plan && !plan_deferred &&
        executor_.tick(*this, planner_iface_->plan_, stride_index_, stride_frame_counter_)) {
        just_applied_controller_move_ = false;
        return;
    }

    if (planner_iface_->has_pending_move()) {
        const auto& req = planner_iface_->pending_move_;
        if (!should_defer_for_non_locked(req.override_non_locked)) {
            apply_pending_move();
            just_applied_controller_move_ = true;
            return;
        }
    }

    if (!got_input && just_applied_controller_move_) {
        auto it = self_->info->animations.find(self_->current_animation);
        if (it != self_->info->animations.end()) {
            Animation& anim = it->second;
            if (!anim.locked) {
                const std::string next_id = anim.on_end_animation.empty()
                                              ? std::string{ animation_update::detail::kDefaultAnimation }
                                              : anim.on_end_animation;
                switch_to(resolve_animation(*self_, next_id), path_index_for(next_id));
            }
        }
        just_applied_controller_move_ = false;
    }

    if (self_->get_current_animation() != animation_update::detail::kDefaultAnimation) {
        if (!advance(self_->current_frame)) {
            switch_to(animation_update::detail::kDefaultAnimation);
            advance(self_->current_frame);
        }
        return;
    }

    advance(self_->current_frame);
}

void AnimationRuntime::apply_pending_move() {
    if (!planner_iface_ || !self_) return;

    const auto req = planner_iface_->consume_move_request();
    const int  resolution = effective_grid_resolution(std::nullopt);
    const SDL_Point from{ self_->world_x(), self_->world_y() };
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

    if (final_position.x != self_->world_x() || final_position.y != self_->world_y()) {
        self_->move_to_world_position(final_position.x, final_position.y, self_->world_z());
        suppress_root_motion_frames_ = std::max(2, suppress_root_motion_frames_);
        if (planner_iface_) {
            planner_iface_->clear_movement_plan();
        }
    }

    planner_iface_->final_dest = self_->world_point();

    const std::string resolved = resolve_animation(*self_, req.animation_id);
    if (self_->current_animation != resolved) {
        switch_to(resolved, path_index_for(resolved));
    } else {
        if (!advance(self_->current_frame)) {
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

    Animation& anim = it->second;
    std::size_t path_index = path_index_for(self_->current_animation);
    if (!frame) {
        frame = anim.get_first_frame(path_index);
        if (!frame) {
            return false;
        }
    }

    const bool is_player = self_->info && self_->info->type == asset_types::player;
    bool should_skip = !is_player && (self_->static_frame || anim.locked || anim.is_frozen());
    bool has_overriding_plan = planner_iface_ && !planner_iface_->plan_.strides.empty() && planner_iface_->plan_.override_non_locked;
    if (should_skip && !has_overriding_plan) {
        self_->static_frame = self_->static_frame || anim.is_frozen() || anim.locked;
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
        if (frame->next) {
            frame = frame->next;
            advanced_any = true;
        } else {
            const bool force_loop_default = self_->current_animation == animation_update::detail::kDefaultAnimation;
            if (anim.loop || force_loop_default) {
                frame = anim.get_first_frame(path_index);
                advanced_any = true;
            } else {
                return false;
            }
        }
    }
    if (advanced_any) {
        self_->mark_composite_dirty();
    }
    return true;
}

void AnimationRuntime::switch_to(const std::string& anim_id, std::size_t path_index) {
    if (!self_ || !self_->info) {
        return;
    }

    const bool animation_changed = self_->current_animation != anim_id;

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
    const int z = self_ ? self_->world_z() : 0;
    const int layer = self_ ? self_->grid_resolution : 0;
    return bottom_middle(world::grid_math::from_sdl(pos, z, layer)).to_sdl_point();
}

bool AnimationRuntime::point_in_impassable(const world::GridPoint& pt, const Asset* ignored) const {
    if (!self_ || !self_->info) {
        return false;
    }
    const Assets* assets = assets_owner_ ? assets_owner_ : (self_ ? self_->get_assets() : nullptr);
    if (!animation_update::detail::bottom_point_inside_playable_area(assets, pt)) {
        return true;
    }
    return visit_impassable_neighbors(*self_, [&](Asset* neighbor) {
        if (!neighbor || neighbor == self_ || neighbor == ignored || !neighbor->info) {
            return false;
        }
        if (neighbor->info->type == asset_types::player) {
            return false;
        }
        Area area = neighbor->get_area("impassable");
        if (area.get_points().empty()) {
            area = neighbor->get_area("collision_area");
        }
        if (area.get_points().empty()) {
            return false;
        }
        return area.contains_point(pt.to_sdl_point());
    });
}

bool AnimationRuntime::point_in_impassable(SDL_Point pt, const Asset* ignored) const {
    const int z = self_ ? self_->world_z() : 0;
    const int layer = self_ ? self_->grid_resolution : 0;
    return point_in_impassable(world::grid_math::from_sdl(pt, z, layer), ignored);
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
    visit_impassable_neighbors(*self_, [&](Asset* neighbor) {
        if (!neighbor || neighbor == self_ || neighbor == ignored || !neighbor->info) {
            return false;
        }
        if (neighbor->info->type == asset_types::player) {
            return false;
        }
        Area area = neighbor->get_area("impassable");
        if (area.get_points().empty()) {
            area = neighbor->get_area("collision_area");
        }
        if (area.get_points().empty()) {
            return false;
        }
        const bool contains_from = area.contains_point(bottom_from.to_sdl_point());
        const bool contains_to   = area.contains_point(dest_bottom.to_sdl_point());
        const bool touches_segment = animation_update::detail::segment_hits_area(from, to, area);
        bool overlaps = false;
        if (!contains_from && !contains_to && !touches_segment) {
            const bool overlap_check = animation_update::detail::should_consider_overlap(*self_, *neighbor);
            if (overlap_check) {
                const world::GridPoint neighbor_bottom = animation_update::detail::bottom_middle_for(*neighbor, world::grid_math::from_sdl(neighbor->world_point(), neighbor->world_z(), neighbor->grid_resolution));
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
    const int z = self_ ? self_->world_z() : 0;
    const int layer = self_ ? self_->grid_resolution : 0;
    const world::GridPoint gp_from = world::grid_math::from_sdl(from, z, layer);
    const world::GridPoint gp_to   = world::grid_math::from_sdl(to, z, layer);
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
        visit_impassable_neighbors(*self_, [&](Asset* neighbor) {
            if (!neighbor || neighbor == self_ || !neighbor->info) {
                return false;
            }
            Area area = neighbor->get_area("impassable");
            if (area.get_points().empty()) {
                area = neighbor->get_area("collision_area");
            }
            if (area.get_points().empty()) {
                return false;
            }
            const bool contains_from = area.contains_point(bottom_from.to_sdl_point());
            const bool contains_to   = area.contains_point(bottom_to.to_sdl_point());
            const bool touches_segment = animation_update::detail::segment_hits_area(from, to, area);
            bool overlaps = false;
            if (!contains_from && !contains_to && !touches_segment) {
                const bool overlap_check = animation_update::detail::should_consider_overlap(*self_, *neighbor);
                if (overlap_check) {
                    const world::GridPoint neighbor_bottom = animation_update::detail::bottom_middle_for(*neighbor, world::grid_math::from_sdl(neighbor->world_point(), neighbor->world_z(), neighbor->grid_resolution));
                    overlaps = animation_update::detail::distance_sq(bottom_from, neighbor_bottom) <
                               animation_update::detail::kOverlapDistanceSq;
                }
            }
            if (!(contains_from || contains_to || touches_segment || overlaps)) {
                return false;
            }
            SDL_Point center = area.get_center();
            push.x += bottom_from.world_x() - center.x;
            push.y += bottom_from.world_y() - center.y;
            blocking_neighbors.push_back(neighbor);
            return false;
        });
    } else {
        for (const Asset* neighbor : blocking_neighbors) {
            if (!neighbor || neighbor == self_ || !neighbor->info) {
                continue;
            }
            Area area = neighbor->get_area("impassable");
            if (area.get_points().empty()) {
                area = neighbor->get_area("collision_area");
            }
            if (area.get_points().empty()) {
                continue;
            }
            SDL_Point center = area.get_center();
            push.x += bottom_from.world_x() - center.x;
            push.y += bottom_from.world_y() - center.y;
        }
    }
    if (push.x == 0 && push.y == 0) {
        push.x = from.world_x() - to.world_x();
        push.y = from.world_y() - to.world_y();
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
        visit_impassable_neighbors(*self_, [&](Asset* neighbor) {
            if (!neighbor || neighbor == self_ || !neighbor->info) return false;
            Area area = neighbor->get_area("impassable");
            if (area.get_points().empty()) area = neighbor->get_area("collision_area");
            if (area.get_points().empty()) return false;
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
        visit_impassable_neighbors(*self_, [&](Asset* neighbor) {
            if (!neighbor || neighbor == self_ || !neighbor->info) return false;
            Area area = neighbor->get_area("impassable");
            if (area.get_points().empty()) area = neighbor->get_area("collision_area");
            if (area.get_points().empty()) return false;
            if (area.contains_point(bottom.to_sdl_point())) { inside = true; return true; }
            return false;
        });
        return inside;
};
    const int max_steps = 12;
    for (SDL_Point dir : directions) {
        world::GridPoint candidate = world::grid_math::from_sdl(self_->world_point(), self_->world_z(), self_->grid_resolution);
        bool      moved     = false;
        for (int step = 0; step < max_steps; ++step) {
            world::GridPoint next = world::grid_math::offset(candidate, dir);
            if (next.world_x() == candidate.world_x() && next.world_y() == candidate.world_y()) continue;
            world::GridPoint bottom_next = animation_update::detail::bottom_middle_for(*self_, next);
            if (inside_disallowed(bottom_next)) break;
            candidate = std::move(next);
            moved = true;
            if (!inside_any(bottom_next)) {
                break;
            }
        }
        if (moved) {
            self_->move_to_world_position(candidate.world_x(), candidate.world_y(), self_->world_z());
            return true;
        }
    }
    return false;
}

bool AnimationRuntime::attempt_unstick(SDL_Point from,
                                       SDL_Point to,
                                       const std::vector<const Asset*>& blockers) {
    const int z = self_ ? self_->world_z() : 0;
    const int layer = self_ ? self_->grid_resolution : 0;
    return attempt_unstick(world::grid_math::from_sdl(from, z, layer),
                           world::grid_math::from_sdl(to, z, layer),
                           blockers);
}

void AnimationRuntime::mark_progress_toward_checkpoints() {
    if (!self_ || !self_->info || !planner_iface_) {
        return;
    }
    const int visited_thresh = planner_iface_->visited_thresh_;
    const int visited_sq     = visited_thresh * visited_thresh;
    while (next_checkpoint_index_ < planner_iface_->plan_.sanitized_checkpoints.size()) {
        const SDL_Point target_sdl  = planner_iface_->plan_.sanitized_checkpoints[next_checkpoint_index_];
        const int z = self_->world_z();
        const int layer = self_->grid_resolution;
        const world::GridPoint current = world::grid_math::from_sdl(self_->world_point(), z, layer);
        const world::GridPoint target  = world::grid_math::from_sdl(target_sdl, z, layer);
        const int       dist_sq = animation_update::detail::distance_sq(current, target);
        bool reached = false;
        if (visited_thresh == 0) {
            reached = (self_->world_x() == target.world_x()) && (self_->world_y() == target.world_y());
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
    mark_progress_toward_checkpoints();
    const int z = self_->world_z();
    const int layer = self_->grid_resolution;
    const SDL_Point target_sdl = (next_checkpoint_index_ < planner_iface_->plan_.sanitized_checkpoints.size()) ? planner_iface_->plan_.sanitized_checkpoints[next_checkpoint_index_] : planner_iface_->final_dest;
    world::GridPoint target = world::grid_math::from_sdl(target_sdl, z, layer);
    world::GridPoint bottom_target = animation_update::detail::bottom_middle_for(*self_, target);
    SDL_Point push{0, 0};
    std::vector<const Asset*> influencing_neighbors;
    auto consider_neighbor = [&](const Asset* neighbor) {
        if (!neighbor || neighbor == self_ || !neighbor->info) return;
        Area area = neighbor->get_area("impassable");
        if (area.get_points().empty()) area = neighbor->get_area("collision_area");
        if (area.get_points().empty()) return;
        bool relevant = area.contains_point(bottom_target.to_sdl_point()) || animation_update::detail::segment_hits_area(world::grid_math::from_sdl(self_->world_point(), z, layer), target, area);
        if (!relevant) {
            const bool overlap_check = animation_update::detail::should_consider_overlap(*self_, *neighbor);
            if (overlap_check) {
                const world::GridPoint neighbor_bottom = animation_update::detail::bottom_middle_for(*neighbor, world::grid_math::from_sdl(neighbor->world_point(), neighbor->world_z(), neighbor->grid_resolution));
                relevant = animation_update::detail::distance_sq(bottom_target, neighbor_bottom) < animation_update::detail::kOverlapDistanceSq;
            }
        }
        if (!relevant) return;
        SDL_Point center = area.get_center();
        push.x += bottom_target.world_x() - center.x;
        push.y += bottom_target.world_y() - center.y;
        influencing_neighbors.push_back(neighbor);
};
    if (!blockers.empty()) {
        for (const Asset* neighbor : blockers) {
            consider_neighbor(neighbor);
        }
    }
    if (influencing_neighbors.empty()) {
        visit_impassable_neighbors(*self_, [&](Asset* neighbor) {
            consider_neighbor(neighbor);
            return false;
        });
    }
    if (push.x == 0 && push.y == 0) {
        push.x = target.world_x() - self_->world_x();
        push.y = target.world_y() - self_->world_y();
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
            if (next.world_x() == candidate.world_x() && next.world_y() == candidate.world_y()) continue;
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

bool AnimationRuntime::handle_blocked_path(const world::GridPoint& from,
                                           const world::GridPoint& to,
                                           const std::vector<const Asset*>& blockers) {
    bool moved = attempt_unstick(from, to, blockers);
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
    const int z = self_ ? self_->world_z() : 0;
    const int layer = self_ ? self_->grid_resolution : 0;
    return handle_blocked_path(world::grid_math::from_sdl(from, z, layer),
                               world::grid_math::from_sdl(to, z, layer),
                               blockers);
}

bool AnimationRuntime::replan_to_destination() {
    if (!self_ || !self_->info || !planner_iface_) {
        return false;
    }
    const int visited_sq = planner_iface_->visited_thresh_ * planner_iface_->visited_thresh_;
    if (visited_sq > 0 && animation_update::detail::distance_sq(self_->world_point(), planner_iface_->final_dest) <= visited_sq) {
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

vibble::grid::Grid& AnimationRuntime::grid() const {
    if (grid_service_) return *grid_service_;
    return vibble::grid::global_grid();
}

int AnimationRuntime::effective_grid_resolution(std::optional<int> override_resolution) const {
    (void)override_resolution;

    return 0;
}

SDL_Point AnimationRuntime::convert_delta_to_world(SDL_Point delta, int resolution) const {
    (void)resolution;
    return delta;
}

bool AnimationRuntime::has_active_plan() const {
    return planner_iface_ && !planner_iface_->current_plan()->strides.empty();
}
