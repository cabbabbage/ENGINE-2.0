#include "animation_update.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <limits>
#include <unordered_set>
#include <unordered_map>
#include <utility>
#include <vector>

#include "assets/asset/Asset.hpp"
#include "assets/asset/animation.hpp"
#include "assets/asset/animation_frame.hpp"
#include "assets/asset/asset_info.hpp"
#include "assets/asset/asset_types.hpp"
#include "animation_runtime.hpp"
#include "movement_rotation.hpp"
#include "animation_tag_utils.hpp"
#include "core/dev_mode_animation_policy.hpp"
#include "movement_target_utils.hpp"
#include "core/AssetsManager.hpp"
#include "gameplay/map_generation/room.hpp"
#include "gameplay/world/grid_point.hpp"
#include "utils/utils/string_utils.hpp"
#include "utils/grid.hpp"
#include "utils/area.hpp"
#include "utils/log.hpp"

namespace {

bool movement_blocked_by_dev_mode(const Assets* assets) {
    return assets && !runtime::dev_mode_policy::should_allow_movement_for_asset(assets->is_dev_mode());
}

struct PlayableRoomsCacheEntry {
    const Room* last_containing_room = nullptr;
    std::unordered_map<const Room*, bool> playable_lookup;
    std::uintptr_t rooms_identity = 0;
    std::size_t    rooms_size     = 0;
};

auto& playable_rooms_cache() {
    static std::unordered_map<const Assets*, PlayableRoomsCacheEntry> cache;
    return cache;
}

bool equals_ignore_case(std::string_view value, std::string_view target) {
    if (value.size() != target.size()) {
        return false;
    }
    for (std::size_t idx = 0; idx < value.size(); ++idx) {
        if (std::tolower(static_cast<unsigned char>(value[idx])) !=
            std::tolower(static_cast<unsigned char>(target[idx]))) {
            return false;
        }
    }
    return true;
}

bool compute_is_playable_room(const Room& room) {
    if (!room.room_area) {
        return false;
    }

    if (equals_ignore_case(room.room_area->get_type(), "room") ||
        equals_ignore_case(room.room_area->get_type(), "trail")) {
        return true;
    }

    return equals_ignore_case(room.type, "room") || equals_ignore_case(room.type, "trail");
}

bool is_playable_room_cached(const Room& room, PlayableRoomsCacheEntry& entry) {
    auto [it, inserted] = entry.playable_lookup.emplace(&room, false);
    if (inserted) {
        it->second = compute_is_playable_room(room);
    }
    return it->second;
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

std::uint64_t auto_move_variance_seed(const Asset& self, std::uint32_t attempt_counter) {
    const std::uint64_t stable_hash = fnv1a_64(animation_update::detail::stable_asset_id(self));
    return mix64(stable_hash ^ (static_cast<std::uint64_t>(attempt_counter) * 0x9e3779b97f4a7c15ULL));
}


int furthest_checkpoint_distance_xz(SDL_Point start, const std::vector<SDL_Point>& checkpoints) {
    int furthest_sq = 0;
    for (const SDL_Point& checkpoint : checkpoints) {
        const int dx = checkpoint.x - start.x;
        const int dz = checkpoint.y - start.y;
        furthest_sq = std::max(furthest_sq, dx * dx + dz * dz);
    }
    return static_cast<int>(std::lround(std::sqrt(static_cast<double>(furthest_sq))));
}

int furthest_checkpoint_distance_xz(const axis::WorldPos& start,
                                    const std::vector<axis::WorldPos>& checkpoints) {
    int furthest_sq = 0;
    for (const axis::WorldPos& checkpoint : checkpoints) {
        const int dx = checkpoint.x - start.x;
        const int dz = checkpoint.z - start.z;
        furthest_sq = std::max(furthest_sq, dx * dx + dz * dz);
    }
    return static_cast<int>(std::lround(std::sqrt(static_cast<double>(furthest_sq))));
}

std::vector<std::string> normalize_tag_list(const std::vector<std::string>& input) {
    std::vector<std::string> normalized;
    normalized.reserve(input.size());
    std::unordered_set<std::string> seen;
    seen.reserve(input.size());

    for (const std::string& raw : input) {
        std::string canonical = vibble::strings::to_lower_copy(vibble::strings::trim_copy(raw));
        if (canonical.empty()) {
            continue;
        }
        if (seen.insert(canonical).second) {
            normalized.push_back(std::move(canonical));
        }
    }

    return normalized;
}

std::mt19937& tag_selection_rng() {
    static std::mt19937 rng{std::random_device{}()};
    return rng;
}

}

namespace animation_update::detail {

std::string stable_asset_id(const Asset& asset) {
    if (!asset.spawn_id.empty()) {
        return asset.spawn_id;
    }
    if (asset.info) {
        return asset.info->name;
    }
    return std::string{};
}

bool should_consider_overlap(const Asset& self, const Asset& other) {
    if (!self.info || !other.info) {
        return false;
    }

    const std::string self_type  = asset_types::canonicalize(self.info->type);
    const std::string other_type = asset_types::canonicalize(other.info->type);

    if (self_type == asset_types::player || other_type == asset_types::player) {
        return false;
    }

    if (self.isMovementEnabled() && other.isMovementEnabled()) {
        return true;
    }

    if (other_type == asset_types::boundary) {
        return true;
    }

    if (other_type == asset_types::enemy || other_type == asset_types::npc) {
        return true;
    }

    if (self_type == other_type && other_type != asset_types::player) {
        return true;
    }

    return false;
}

int overlap_distance_sq_for_pair(const Asset& self,
                                 const Asset& other,
                                 const PathBlockingContext& context) {
    if (!should_consider_overlap(self, other)) {
        return 0;
    }

    constexpr int kEnemyEnemySpacingPx = 56;
    constexpr int kEnemyNpcSpacingPx = 44;
    constexpr int kDefaultSpacingPx = 40;
    constexpr int kEngagementTargetSpacingPx = 18;

    const std::string self_type = self.info ? asset_types::canonicalize(self.info->type) : std::string{};
    const std::string other_type = other.info ? asset_types::canonicalize(other.info->type) : std::string{};

    if (context.engagement_target_asset_id.has_value()) {
        const std::string other_id = stable_asset_id(other);
        if (!other_id.empty() && other_id == *context.engagement_target_asset_id) {
            if (context.allow_engagement_target_overlap) {
                return 0;
            }
            return kEngagementTargetSpacingPx * kEngagementTargetSpacingPx;
        }
    }

    if (self_type == asset_types::enemy && other_type == asset_types::enemy) {
        return kEnemyEnemySpacingPx * kEnemyEnemySpacingPx;
    }
    if ((self_type == asset_types::enemy && other_type == asset_types::npc) ||
        (self_type == asset_types::npc && other_type == asset_types::enemy)) {
        return kEnemyNpcSpacingPx * kEnemyNpcSpacingPx;
    }

    return kDefaultSpacingPx * kDefaultSpacingPx;
}

int distance_sq(const world::GridPoint& a, const world::GridPoint& b) {
    return world::grid_math::distance_sq(a, b);
}

int distance_sq(SDL_Point a, SDL_Point b) {
    return distance_sq(world::grid_math::from_sdl(a), world::grid_math::from_sdl(b));
}

long long distance_sq_3d(const world::GridPoint& a, const world::GridPoint& b) {
    const long long dx = static_cast<long long>(a.world_x()) - static_cast<long long>(b.world_x());
    const long long dy = static_cast<long long>(a.world_y()) - static_cast<long long>(b.world_y());
    const long long dz = static_cast<long long>(a.world_z()) - static_cast<long long>(b.world_z());
    return dx * dx + dy * dy + dz * dz;
}

long long distance_sq_3d(const axis::WorldPos& a, const axis::WorldPos& b) {
    const long long dx = static_cast<long long>(a.x) - static_cast<long long>(b.x);
    const long long dy = static_cast<long long>(a.y) - static_cast<long long>(b.y);
    const long long dz = static_cast<long long>(a.z) - static_cast<long long>(b.z);
    return dx * dx + dy * dy + dz * dz;
}

bool segment_hits_area(const world::GridPoint& from_gp,
                       const world::GridPoint& to_gp,
                       const Area& area) {
    const SDL_Point from = from_gp.to_sdl_point();
    const SDL_Point to   = to_gp.to_sdl_point();
    const int steps = std::max(std::abs(to.x - from.x), std::abs(to.y - from.y));
    if (steps == 0) {
        return area.contains_point(from_gp.to_sdl_point());
    }

    const double step_x = (to.x - from.x) / static_cast<double>(steps);
    const double step_y = (to.y - from.y) / static_cast<double>(steps);

    for (int i = 0; i <= steps; ++i) {
        SDL_Point sample{ static_cast<int>(std::round(from.x + step_x * i)),
                          static_cast<int>(std::round(from.y + step_y * i)) };
        if (area.contains_point(sample)) {
            return true;
        }
    }
    return false;
}

bool segment_hits_area(SDL_Point from, SDL_Point to, const Area& area) {
    return segment_hits_area(world::grid_math::from_sdl(from),
                             world::grid_math::from_sdl(to),
                             area);
}

world::GridPoint bottom_middle_for(const Asset& asset, const world::GridPoint& pos) {
    Area        area = asset.get_area("collision_area");
    const auto& pts  = area.get_points();
    if (pts.empty()) {
        return pos;
    }

    SDL_Point bottom = pts.front();
    for (const SDL_Point& pt : pts) {
        if (pt.y > bottom.y) {
            bottom = pt;
        }
    }

    const int offset_x = bottom.x - asset.world_x();
    const int offset_y = bottom.y - asset.world_z();
    return world::grid_math::offset(pos, offset_x, offset_y);
}

SDL_Point bottom_middle_for(const Asset& asset, SDL_Point pos) {
    return bottom_middle_for(asset,
                             world::grid_math::from_sdl(pos, asset.world_y(), asset.grid_resolution))
        .to_sdl_point();
}

SDL_Point frame_world_delta(const AnimationFrame& frame,
                            const Asset&          asset,
                            const vibble::grid::Grid& grid) {
    (void)asset;
    (void)grid;
    return animation_update::movement_rotation::frame_floor_delta_absolute_yaw(frame);
}

axis::WorldPos frame_world_delta_3d(const AnimationFrame& frame,
                                    const Asset&          asset,
                                    const vibble::grid::Grid& grid) {
    (void)asset;
    (void)grid;
    return animation_update::movement_rotation::frame_world_delta_absolute_yaw(frame);
}

bool bottom_point_inside_playable_area(const Assets* assets, const world::GridPoint& bottom_point) {
    if (!assets) {
        return false;
    }

    auto& cache_entry = playable_rooms_cache()[assets];

    const std::vector<Room*>& rooms = assets->rooms();
    const std::uintptr_t identity = rooms.empty() ? 0 : reinterpret_cast<std::uintptr_t>(rooms.data());
    if (cache_entry.rooms_identity != identity || cache_entry.rooms_size != rooms.size()) {
        cache_entry.rooms_identity        = identity;
        cache_entry.rooms_size            = rooms.size();
        cache_entry.last_containing_room  = nullptr;
        cache_entry.playable_lookup.clear();
    }

    auto contains_playable = [&](const Room* room) -> bool {
        if (!room || !room->room_area) {
            return false;
        }
        if (!is_playable_room_cached(*room, cache_entry)) {
            return false;
        }
        return room->room_area->contains_point(bottom_point.to_sdl_point());
};

    if (cache_entry.last_containing_room && contains_playable(cache_entry.last_containing_room)) {
        return true;
    }

    for (const Room* room : rooms) {
        if (contains_playable(room)) {
            cache_entry.last_containing_room = room;
            return true;
        }
    }

    cache_entry.last_containing_room = nullptr;
    return false;
}

bool bottom_point_inside_playable_area(const Assets* assets, SDL_Point bottom_point) {
    return bottom_point_inside_playable_area(assets, world::grid_math::from_sdl(bottom_point));
}

bool segment_leaves_playable_area(const Assets* assets,
                                  const world::GridPoint& from_gp,
                                  const world::GridPoint& to_gp) {
    if (!assets) {
        return false;
    }

    const bool start_inside = bottom_point_inside_playable_area(assets, from_gp);
    const bool end_inside   = bottom_point_inside_playable_area(assets, to_gp);

    if (!start_inside || !end_inside) {
        return true;
    }

    const SDL_Point from = from_gp.to_sdl_point();
    const SDL_Point to   = to_gp.to_sdl_point();
    const int steps = std::max(std::abs(to.x - from.x), std::abs(to.y - from.y));
    if (steps <= 1) {
        return false;
    }

    const double step_x = (to.x - from.x) / static_cast<double>(steps);
    const double step_y = (to.y - from.y) / static_cast<double>(steps);

    for (int i = 1; i < steps; ++i) {
        SDL_Point sample{ static_cast<int>(std::round(from.x + step_x * i)),
                          static_cast<int>(std::round(from.y + step_y * i)) };
        if (!bottom_point_inside_playable_area(assets, sample)) {
            return true;
        }
    }

    return false;
}

bool segment_leaves_playable_area(const Assets* assets, SDL_Point from, SDL_Point to) {
    return segment_leaves_playable_area(assets,
                                        world::grid_math::from_sdl(from),
                                        world::grid_math::from_sdl(to));
}

}

AnimationUpdate::AnimationUpdate(Asset* self, Assets* assets)
    : self_(self), assets_owner_(assets), grid_service_(&vibble::grid::global_grid()) {
    if (!assets_owner_ && self_) {
        assets_owner_ = self_->get_assets();
    }
}

void AnimationUpdate::auto_move(SDL_Point world_checkpoint,
                                int visited_thresh_px,
                                std::optional<int> checkpoint_resolution,
                                bool override_non_locked,
                                AutoMoveCombatOverrides combat_overrides) {
    if (!self_) {
        return;
    }
    if (!self_->isMovementEnabled()) {
        clear_movement_plan();
        return;
    }
    if (movement_blocked_by_dev_mode(assets_owner_)) {
        clear_movement_plan();
        return;
    }
    if (should_defer_auto_move_for_committed_attack()) {
        return;
    }
    auto_move_attacking_enabled_ =
        resolve_auto_move_combat_options(combat_overrides).attacking_enabled;
    SDL_Point delta = animation_update::movement_targets::world_delta_to_checkpoint(*self_, world_checkpoint);
    if (delta.x == 0 && delta.y == 0) {
        self_->target_reached = true;
        self_->needs_target = true;
        return;
    }
    std::vector<SDL_Point> rel{ delta };
    auto_move(rel, visited_thresh_px, checkpoint_resolution, override_non_locked, combat_overrides);
}

void AnimationUpdate::auto_move(Asset* target_asset,
                                int visited_thresh_px,
                                bool override_non_locked,
                                AutoMoveCombatOverrides combat_overrides) {
    if (!self_ || !target_asset) {
        return;
    }
    if (!self_->isMovementEnabled()) {
        clear_movement_plan();
        return;
    }
    if (movement_blocked_by_dev_mode(assets_owner_)) {
        clear_movement_plan();
        return;
    }
    if (should_defer_auto_move_for_committed_attack()) {
        return;
    }
    if (self_) {
        self_->target_reached = false;
    }
    const AutoMoveCombatOptions combat_options = resolve_auto_move_combat_options(combat_overrides);
    const MovementTagFilter movement_tag_filter = resolve_movement_tag_filter(combat_overrides);
    auto_move_attacking_enabled_ = combat_options.attacking_enabled;
    const SDL_Point checkpoint = animation_update::movement_targets::world_checkpoint(*target_asset);
    SDL_Point delta = animation_update::movement_targets::world_delta_to_checkpoint(*self_, checkpoint);
    if (delta.x == 0 && delta.y == 0) {
        if (self_) {
            self_->target_reached = true;
            self_->needs_target = true;
        }
        return;
    }
    const std::string target_asset_id = animation_update::detail::stable_asset_id(*target_asset);
    pending_engagement_target_asset_id_ =
        target_asset_id.empty() ? std::nullopt : std::make_optional(target_asset_id);
    auto_move(checkpoint, visited_thresh_px, std::nullopt, override_non_locked, combat_overrides);
    pending_engagement_target_asset_id_ = std::nullopt;
}

void AnimationUpdate::auto_move_3d(axis::WorldPos world_checkpoint,
                                   int            visited_thresh_px,
                                   std::optional<int> checkpoint_resolution,
                                   bool           override_non_locked,
                                   AutoMoveCombatOverrides combat_overrides) {
    if (!self_) {
        return;
    }
    if (!self_->isMovementEnabled()) {
        clear_movement_plan();
        return;
    }
    if (movement_blocked_by_dev_mode(assets_owner_)) {
        clear_movement_plan();
        return;
    }
    if (should_defer_auto_move_for_committed_attack()) {
        return;
    }
    auto_move_attacking_enabled_ =
        resolve_auto_move_combat_options(combat_overrides).attacking_enabled;

    const axis::WorldPos delta =
        animation_update::movement_targets::world_delta_to_checkpoint_3d(*self_, world_checkpoint);
    if (delta.x == 0 && delta.y == 0 && delta.z == 0) {
        self_->target_reached = true;
        self_->needs_target = true;
        return;
    }

    auto_move_3d(std::vector<axis::WorldPos>{ delta },
                 true,
                 visited_thresh_px,
                 checkpoint_resolution,
                 override_non_locked,
                 combat_overrides);
}

void AnimationUpdate::auto_move_3d(Asset* target_asset,
                                   int visited_thresh_px,
                                   bool override_non_locked,
                                   AutoMoveCombatOverrides combat_overrides) {
    if (!self_ || !target_asset) {
        return;
    }
    if (!self_->isMovementEnabled()) {
        clear_movement_plan();
        return;
    }
    if (movement_blocked_by_dev_mode(assets_owner_)) {
        clear_movement_plan();
        return;
    }
    if (should_defer_auto_move_for_committed_attack()) {
        return;
    }

    const AutoMoveCombatOptions combat_options = resolve_auto_move_combat_options(combat_overrides);
    auto_move_attacking_enabled_ = combat_options.attacking_enabled;
    self_->target_reached = false;
    const axis::WorldPos checkpoint = animation_update::movement_targets::world_checkpoint_3d(*target_asset);
    const axis::WorldPos delta = animation_update::movement_targets::world_delta_to_checkpoint_3d(*self_, checkpoint);
    if (delta.x == 0 && delta.y == 0 && delta.z == 0) {
        self_->target_reached = true;
        self_->needs_target = true;
        return;
    }

    const std::string target_asset_id = animation_update::detail::stable_asset_id(*target_asset);
    pending_engagement_target_asset_id_ =
        target_asset_id.empty() ? std::nullopt : std::make_optional(target_asset_id);
    auto_move_3d(checkpoint, visited_thresh_px, std::nullopt, override_non_locked, combat_overrides);
    pending_engagement_target_asset_id_ = std::nullopt;
}

void AnimationUpdate::auto_move_3d_relative(axis::WorldPos rel_delta,
                                            int            visited_thresh_px,
                                            std::optional<int> checkpoint_resolution,
                                            bool           override_non_locked,
                                            AutoMoveCombatOverrides combat_overrides) {
    auto_move_3d(std::vector<axis::WorldPos>{ rel_delta },
                 true,
                 visited_thresh_px,
                 checkpoint_resolution,
                 override_non_locked,
                 combat_overrides);
}

void AnimationUpdate::auto_move_3d(const std::vector<axis::WorldPos>& checkpoints,
                                   bool                               relative_checkpoints,
                                   int                                visited_thresh_px,
                                   std::optional<int>                 checkpoint_resolution,
                                   bool                               override_non_locked,
                                   AutoMoveCombatOverrides            combat_overrides) {
    if (!self_) {
        return;
    }
    if (!self_->isMovementEnabled()) {
        clear_movement_plan();
        return;
    }
    if (movement_blocked_by_dev_mode(assets_owner_)) {
        clear_movement_plan();
        return;
    }
    if (should_defer_auto_move_for_committed_attack()) {
        return;
    }

    const std::uint32_t plan_frame_id = resolve_plan_frame_id();
    if (planning_retry_cooldown_active(plan_frame_id)) {
        active_plan_mode_ = ActivePlanMode::None;
        if (self_) {
            self_->target_reached = false;
            self_->needs_target = true;
        }
        return;
    }

    if (self_) {
        self_->target_reached = false;
    }
    const AutoMoveCombatOptions combat_options = resolve_auto_move_combat_options(combat_overrides);
    const MovementTagFilter movement_tag_filter = resolve_movement_tag_filter(combat_overrides);
    auto_move_attacking_enabled_ = combat_options.attacking_enabled;

    const std::string asset_name = self_->info ? self_->info->name : std::string{"<unknown>"};
    const int resolution = effective_grid_resolution(checkpoint_resolution);
    visited_thresh_      = std::max(0, visited_thresh_px);
    if (resolution > 0) {
        const int step = vibble::grid::delta(resolution);
        if (step > 1 && visited_thresh_ > 0) {
            visited_thresh_ = ((visited_thresh_ + step - 1) / step) * step;
        }
    }

    const bool debug_logging = debug_enabled_;
    if (debug_logging) {
        std::ostringstream oss;
        oss << "[AnimationUpdate] auto_move_3d asset=" << asset_name
            << " checkpoints=" << checkpoints.size()
            << " relative=" << std::boolalpha << relative_checkpoints
            << " visited_thresh=" << visited_thresh_
            << " override_non_locked=" << override_non_locked;
        vibble::log::info(oss.str());
    }

    std::vector<axis::WorldPos> absolute;
    absolute.reserve(checkpoints.size());
    if (relative_checkpoints) {
        vibble::grid::Grid& grid_service = grid();
        SDL_Point           cursor_index = grid_service.world_to_index(self_->world_xz_point(), resolution);
        int                 cursor_y     = self_->world_y();
        for (const axis::WorldPos& delta : checkpoints) {
            const SDL_Point delta_xz{ delta.x, delta.z };
            const SDL_Point delta_indices = grid_service.convert_resolution(delta_xz, 0, resolution);
            cursor_index.x += delta_indices.x;
            cursor_index.y += delta_indices.y;
            cursor_y += delta.y;
            const SDL_Point next_world = grid_service.index_to_world(cursor_index, resolution);
            absolute.push_back(axis::WorldPos{ next_world.x, cursor_y, next_world.y });
        }
    } else {
        absolute = checkpoints;
    }

    const std::vector<axis::WorldPos> requested_absolute = absolute;
    const int furthest_checkpoint_distance_px =
        furthest_checkpoint_distance_xz(axis::WorldPos{ self_->world_x(), self_->world_y(), self_->world_z() },
                                        requested_absolute);
    if (furthest_checkpoint_distance_px > 0 && visited_thresh_ >= furthest_checkpoint_distance_px) {
        visited_thresh_ = furthest_checkpoint_distance_px - 1;
    }
    CollisionQueryContext collision_context;
    collision_context.engagement_target_asset_id = pending_engagement_target_asset_id_;
    collision_context.required_animation_tags = movement_tag_filter.required_tags;
    collision_context.excluded_animation_tags = movement_tag_filter.excluded_tags;
    collision_context.path_variance_seed = auto_move_variance_seed(*self_, ++plan_variance_attempt_counter_);
    collision_context.set_furthest_checkpoint_distance_px(furthest_checkpoint_distance_px);
    const std::vector<axis::WorldPos> sanitized_checkpoints =
        sanitizer_3d_.sanitize(*self_, requested_absolute, visited_thresh_);
    plan3d_ = planner_3d_(*self_, sanitized_checkpoints, visited_thresh_, grid(), &collision_context);
    plan3d_.world_start = axis::WorldPos{ self_->world_x(), self_->world_y(), self_->world_z() };
    plan3d_.override_non_locked = override_non_locked;
    plan3d_.engagement_target_asset_id = pending_engagement_target_asset_id_;
    plan3d_.attacking_enabled = combat_options.attacking_enabled;
    plan3d_.movement_tag_filter = movement_tag_filter;
    final_dest_3d = plan3d_.final_dest;

    // 3D plan runs in its own mode and must not reuse stale 2D plan state.
    plan_.strides.clear();
    plan_.sanitized_checkpoints.clear();
    plan_.final_dest = self_->world_xz_point();
    plan_.world_start = self_->world_xz_point();
    plan_.engagement_target_asset_id = std::nullopt;
    plan_.attacking_enabled = false;
    plan_.movement_tag_filter = {};
    final_dest = plan_.final_dest;

    if (debug_logging) {
        std::ostringstream oss;
        oss << "[AnimationUpdate] auto_move_3d plan asset=" << asset_name
            << " final_dest=(" << final_dest_3d.x << "," << final_dest_3d.y << "," << final_dest_3d.z << ")"
            << " sanitized_points=" << plan3d_.sanitized_checkpoints.size()
            << " strides=" << plan3d_.strides.size();
        vibble::log::info(oss.str());
    }

    if (plan3d_.strides.empty()) {
        if (debug_logging) {
            vibble::log::info("[AnimationUpdate] auto_move_3d plan produced no strides for asset=" + asset_name);
        }
        arm_plan_retry_cooldown(plan_frame_id);
        active_plan_mode_ = ActivePlanMode::None;
        if (self_) {
            self_->needs_target = true;
        }
        return;
    }

    if (runtime_) {
        runtime_->reset_plan_progress();
    }

    active_plan_mode_ = ActivePlanMode::Plan3D;
    clear_plan_retry_cooldown();
    input_event_ = true;
    if (self_) {
        self_->needs_target = false;
    }
}

void AnimationUpdate::auto_move(const std::vector<SDL_Point>& rel_checkpoints,
                                int visited_thresh_px,
                                std::optional<int> checkpoint_resolution,
                                bool override_non_locked,
                                AutoMoveCombatOverrides combat_overrides) {
    if (!self_) {
        return;
    }
    if (!self_->isMovementEnabled()) {
        clear_movement_plan();
        return;
    }
    if (movement_blocked_by_dev_mode(assets_owner_)) {
        clear_movement_plan();
        return;
    }
    if (should_defer_auto_move_for_committed_attack()) {
        return;
    }

    const std::uint32_t plan_frame_id = resolve_plan_frame_id();
    if (planning_retry_cooldown_active(plan_frame_id)) {
        active_plan_mode_ = ActivePlanMode::None;
        if (self_) {
            self_->target_reached = false;
            self_->needs_target = true;
        }
        return;
    }

    const std::string asset_name = self_->info ? self_->info->name : std::string{"<unknown>"};
    const AutoMoveCombatOptions combat_options = resolve_auto_move_combat_options(combat_overrides);
    const MovementTagFilter movement_tag_filter = resolve_movement_tag_filter(combat_overrides);
    auto_move_attacking_enabled_ = combat_options.attacking_enabled;
    const int resolution = effective_grid_resolution(checkpoint_resolution);
    visited_thresh_      = std::max(0, visited_thresh_px);
    if (resolution > 0) {
        const int step = vibble::grid::delta(resolution);
        if (step > 1 && visited_thresh_ > 0) {
            visited_thresh_ = ((visited_thresh_ + step - 1) / step) * step;
        }
    }
    const bool debug_logging = debug_enabled_;
    if (debug_logging) {
        std::ostringstream oss;
        oss << "[AnimationUpdate] auto_move asset=" << asset_name
            << " rel_checkpoints=" << rel_checkpoints.size() << " visited_thresh=" << visited_thresh_ << " override_non_locked=" << std::boolalpha << override_non_locked;
        vibble::log::info(oss.str());
    }

    std::vector<SDL_Point> absolute;
    absolute.reserve(rel_checkpoints.size());
    vibble::grid::Grid& grid_service = grid();
    SDL_Point           cursor_index = grid_service.world_to_index(self_->world_xz_point(), resolution);
    for (const SDL_Point& delta : rel_checkpoints) {
        SDL_Point delta_indices = grid_service.convert_resolution(delta, 0, resolution);
        cursor_index.x += delta_indices.x;
        cursor_index.y += delta_indices.y;
        SDL_Point next_world = grid_service.index_to_world(cursor_index, resolution);
        absolute.push_back(next_world);
    }

    const std::vector<SDL_Point> requested_absolute = absolute;
    const int furthest_checkpoint_distance_px =
        furthest_checkpoint_distance_xz(self_->world_xz_point(), requested_absolute);
    if (furthest_checkpoint_distance_px > 0 && visited_thresh_ >= furthest_checkpoint_distance_px) {
        visited_thresh_ = furthest_checkpoint_distance_px - 1;
    }
    CollisionQueryContext collision_context;
    collision_context.engagement_target_asset_id = pending_engagement_target_asset_id_;
    collision_context.required_animation_tags = movement_tag_filter.required_tags;
    collision_context.excluded_animation_tags = movement_tag_filter.excluded_tags;
    collision_context.path_variance_seed = auto_move_variance_seed(*self_, ++plan_variance_attempt_counter_);
    collision_context.set_furthest_checkpoint_distance_px(furthest_checkpoint_distance_px);
    const std::vector<SDL_Point> sanitized_checkpoints =
        sanitizer_.sanitize(*self_, requested_absolute, visited_thresh_, &collision_context);
    plan_      = planner_(*self_, sanitized_checkpoints, visited_thresh_, grid(), &collision_context);
    final_dest = plan_.final_dest;
    plan_.world_start = self_->world_xz_point();
    plan_.override_non_locked = override_non_locked;
    plan_.engagement_target_asset_id = pending_engagement_target_asset_id_;
    plan_.attacking_enabled = combat_options.attacking_enabled;
    plan_.movement_tag_filter = movement_tag_filter;

    // 2D plan runs in its own mode and must not reuse stale 3D plan state.
    plan3d_.strides.clear();
    plan3d_.sanitized_checkpoints.clear();
    plan3d_.final_dest = axis::WorldPos{ self_->world_x(), self_->world_y(), self_->world_z() };
    plan3d_.world_start = plan3d_.final_dest;
    plan3d_.engagement_target_asset_id = std::nullopt;
    plan3d_.attacking_enabled = false;
    plan3d_.movement_tag_filter = {};
    final_dest_3d = plan3d_.final_dest;

    if (debug_logging) {
        std::ostringstream oss;
        oss << "[AnimationUpdate] auto_move plan asset=" << asset_name
            << " final_dest=(" << final_dest.x << "," << final_dest.y << ")"
            << " sanitized_points=" << plan_.sanitized_checkpoints.size() << " strides=" << plan_.strides.size();
        vibble::log::info(oss.str());
    }

    if (plan_.strides.empty()) {
        if (debug_logging) {
            vibble::log::info("[AnimationUpdate] auto_move plan produced no strides for asset=" + asset_name);
        }
        arm_plan_retry_cooldown(plan_frame_id);
        active_plan_mode_ = ActivePlanMode::None;
        if (self_) {
            self_->needs_target = true;
        }
        return;
    }

    if (runtime_) {
        runtime_->reset_plan_progress();
    }

    active_plan_mode_ = ActivePlanMode::Plan2D;
    clear_plan_retry_cooldown();
    input_event_ = true;
    if (self_) {
        self_->needs_target = false;
    }
}

void AnimationUpdate::move(SDL_Point delta,
                           const std::string& animation,
                           bool               resort_z,
                           bool               override_non_locked) {
    if (!self_ || !self_->info) {
        return;
    }
    if (!self_->isMovementEnabled()) {
        clear_movement_plan();
        return;
    }
    if (movement_blocked_by_dev_mode(assets_owner_)) {
        clear_movement_plan();
        return;
    }

    pending_move_.delta        = delta;
    pending_move_.animation_id = animation;
    pending_move_.resort_z     = resort_z;
    pending_move_.override_non_locked = override_non_locked;
    move_pending_              = true;
    input_event_               = true;
    clear_plan_retry_cooldown();
}

void AnimationUpdate::move_3d(const axis::WorldPos& delta,
                              const std::string&    animation,
                              bool                  resort_z,
                              bool                  override_non_locked) {
    if (!self_ || !self_->info) {
        return;
    }
    if (!self_->isMovementEnabled()) {
        clear_movement_plan();
        return;
    }
    if (movement_blocked_by_dev_mode(assets_owner_)) {
        clear_movement_plan();
        return;
    }

    pending_move_3d_.delta = delta;
    pending_move_3d_.animation_id = animation;
    pending_move_3d_.resort_z = resort_z;
    pending_move_3d_.override_non_locked = override_non_locked;
    move_pending_3d_ = true;
    input_event_ = true;
    clear_plan_retry_cooldown();
}


void AnimationUpdate::clear_movement_plan() {
    const std::string asset_name = self_ && self_->info ? self_->info->name : std::string{"<unknown>"};
    const bool debug_logging = debug_enabled_;

    plan_.strides.clear();
    plan_.sanitized_checkpoints.clear();
    plan_.final_dest = self_ ? self_->world_xz_point() : SDL_Point{ 0, 0 };
    plan_.world_start = plan_.final_dest;
    plan_.engagement_target_asset_id = std::nullopt;
    plan_.override_non_locked = true;
    plan_.attacking_enabled = false;
    plan_.movement_tag_filter = {};
    final_dest = plan_.final_dest;

    plan3d_.strides.clear();
    plan3d_.sanitized_checkpoints.clear();
    plan3d_.final_dest = self_
        ? axis::WorldPos{ self_->world_x(), self_->world_y(), self_->world_z() }
        : axis::WorldPos{ 0, 0, 0 };
    plan3d_.world_start = plan3d_.final_dest;
    plan3d_.engagement_target_asset_id = std::nullopt;
    plan3d_.override_non_locked = true;
    plan3d_.attacking_enabled = false;
    plan3d_.movement_tag_filter = {};
    final_dest_3d = plan3d_.final_dest;

    active_plan_mode_ = ActivePlanMode::None;
    auto_move_attacking_enabled_ = false;
    input_event_ = true;
    clear_plan_retry_cooldown();

    if (debug_logging) {
        std::ostringstream oss;
        oss << "[AnimationUpdate] clear_movement_plan asset=" << asset_name
            << " final_dest_2d=(" << final_dest.x << "," << final_dest.y << ")"
            << " final_dest_3d=(" << final_dest_3d.x << "," << final_dest_3d.y << "," << final_dest_3d.z << ")";
        vibble::log::info(oss.str());
    }

    if (runtime_) {
        runtime_->reset_plan_progress();
    }
    if (self_) {
        self_->needs_target = true;
    }
}

void AnimationUpdate::cancel_all_movement() {
    move_pending_3d_ = false;
    pending_move_3d_ = MoveRequest3D{};
    clear_movement_plan();
    move(SDL_Point{0, 0}, animation_update::detail::kDefaultAnimation, true, true);
}

void AnimationUpdate::stop_movement() {
    move_pending_ = false;
    pending_move_ = MoveRequest{};
    move_pending_3d_ = false;
    pending_move_3d_ = MoveRequest3D{};
    clear_movement_plan();
}


void AnimationUpdate::set_debug_enabled(bool enabled) {
    debug_enabled_ = enabled;
    if (runtime_) {
        runtime_->set_debug_enabled(enabled);
    }
}

bool AnimationUpdate::debug_enabled() const {
    return debug_enabled_;
}

std::uint32_t AnimationUpdate::resolve_plan_frame_id() {
    if (assets_owner_) {
        return assets_owner_->frame_id();
    }
    ++local_plan_frame_counter_;
    if (local_plan_frame_counter_ == 0) {
        ++local_plan_frame_counter_;
    }
    return local_plan_frame_counter_;
}

bool AnimationUpdate::planning_retry_cooldown_active(std::uint32_t frame_id) const {
    return next_plan_retry_frame_ != 0 && frame_id < next_plan_retry_frame_;
}

void AnimationUpdate::arm_plan_retry_cooldown(std::uint32_t frame_id) {
    if (frame_id == std::numeric_limits<std::uint32_t>::max()) {
        next_plan_retry_frame_ = frame_id;
        return;
    }
    const std::uint64_t next =
        static_cast<std::uint64_t>(frame_id) + static_cast<std::uint64_t>(kPlanRetryCooldownFrames) + 1ULL;
    next_plan_retry_frame_ = static_cast<std::uint32_t>(
        std::min<std::uint64_t>(next, std::numeric_limits<std::uint32_t>::max()));
}

void AnimationUpdate::clear_plan_retry_cooldown() {
    next_plan_retry_frame_ = 0;
}

vibble::grid::Grid& AnimationUpdate::grid() const {
    if (grid_service_) {
        return *grid_service_;
    }
    return vibble::grid::global_grid();
}

int AnimationUpdate::effective_grid_resolution(std::optional<int> override_resolution) const {
    return resolve_effective_grid_resolution(self_, grid(), override_resolution);
}
