#pragma once

#include <optional>
#include <string>
#include <cstdint>
#include <vector>

#include <SDL3/SDL.h>

#include "stride_types.hpp"
#include "path_sanitizer.hpp"
#include "path_sanitizer_3d.hpp"
#include "get_best_path.hpp"
#include "get_best_path_3d.hpp"
#include "rendering/render/warped_screen_grid.hpp"
#include "assets/asset/animation.hpp"
#include "core/axis_convention.hpp"
#include "gameplay/world/grid_point.hpp"

class Area;
class Asset;
class Assets;
class AnimationFrame;

namespace vibble::grid {
class Grid;
}

class Animation;
class AnimationRuntime;

namespace animation_update::detail {

inline constexpr const char kDefaultAnimation[] = "default";
inline constexpr int        kOverlapDistanceSq  = 40 * 40;

struct PathBlockingContext {
    std::optional<std::string> engagement_target_asset_id = std::nullopt;
    bool allow_engagement_target_overlap = false;
};

std::string stable_asset_id(const Asset& asset);
bool should_consider_overlap(const Asset& self, const Asset& other);
int overlap_distance_sq_for_pair(const Asset& self,
                                 const Asset& other,
                                 const PathBlockingContext& context = {});
int  distance_sq(const world::GridPoint& a, const world::GridPoint& b);
int  distance_sq(SDL_Point a, SDL_Point b);
long long distance_sq_3d(const world::GridPoint& a, const world::GridPoint& b);
long long distance_sq_3d(const axis::WorldPos& a, const axis::WorldPos& b);
bool segment_hits_area(const world::GridPoint& from, const world::GridPoint& to, const Area& area);
bool segment_hits_area(SDL_Point from, SDL_Point to, const Area& area);
world::GridPoint bottom_middle_for(const Asset& asset, const world::GridPoint& pos);
SDL_Point bottom_middle_for(const Asset& asset, SDL_Point pos);
SDL_Point frame_world_delta(const AnimationFrame& frame, const Asset&          asset, const vibble::grid::Grid& grid);
axis::WorldPos frame_world_delta_3d(const AnimationFrame& frame, const Asset& asset, const vibble::grid::Grid& grid);
bool bottom_point_inside_playable_area(const Assets* assets, const world::GridPoint& bottom_point);
bool bottom_point_inside_playable_area(const Assets* assets, SDL_Point bottom_point);
bool segment_leaves_playable_area(const Assets* assets, const world::GridPoint& from, const world::GridPoint& to);
bool segment_leaves_playable_area(const Assets* assets, SDL_Point from, SDL_Point to);

}

class AnimationUpdate {
public:
    struct AutoMoveCombatOptions {
        bool attacking_enabled = false;
    };

    struct AutoMoveCombatOverrides {
        std::optional<bool> attacking_enabled = std::nullopt;
        std::optional<bool> force_attacking_enabled = std::nullopt;
        std::vector<std::string> required_movement_tags{};
        std::vector<std::string> excluded_movement_tags{};
    };

    enum class ReversePlaybackCommand {
        None,
        ReverseUntilStopCurrentAnimation,
        ReverseToDefaultAtStart,
        Stop,
    };
    enum class ActivePlanMode {
        None = 0,
        Plan2D,
        Plan3D
    };

    AnimationUpdate(Asset* self, Assets* assets);
    void set_debug_enabled(bool enabled);
    bool debug_enabled() const;

    void auto_move(const std::vector<SDL_Point>& rel_checkpoints, int visited_thresh_px, std::optional<int> checkpoint_resolution = std::nullopt, bool override_non_locked = true, AutoMoveCombatOverrides combat_overrides = {});
    void auto_move(SDL_Point world_checkpoint, int visited_thresh_px = 0, std::optional<int> checkpoint_resolution = std::nullopt, bool override_non_locked = true, AutoMoveCombatOverrides combat_overrides = {});
    void auto_move(Asset* target_asset, int visited_thresh_px = 0, bool override_non_locked = true, AutoMoveCombatOverrides combat_overrides = {});
    void auto_move_3d(axis::WorldPos world_checkpoint, int visited_thresh_px = 0, std::optional<int> checkpoint_resolution = std::nullopt, bool override_non_locked = true, AutoMoveCombatOverrides combat_overrides = {});
    void auto_move_3d(Asset* target_asset, int visited_thresh_px = 0, bool override_non_locked = true, AutoMoveCombatOverrides combat_overrides = {});
    void auto_move_3d_relative(axis::WorldPos rel_delta, int visited_thresh_px = 0, std::optional<int> checkpoint_resolution = std::nullopt, bool override_non_locked = true, AutoMoveCombatOverrides combat_overrides = {});
    void auto_move_3d(const std::vector<axis::WorldPos>& checkpoints,
                      bool                               relative_checkpoints,
                      int                                visited_thresh_px,
                      std::optional<int>                 checkpoint_resolution = std::nullopt,
                      bool                               override_non_locked   = true,
                      AutoMoveCombatOverrides            combat_overrides      = {});

    int visit_threshold_px() const { return visited_thresh_; }

    void move(SDL_Point delta, const std::string& animation, bool               resort_z            = true, bool               override_non_locked = true);
    void move_3d(const axis::WorldPos& delta, const std::string& animation, bool resort_z = true, bool override_non_locked = true);

    bool set_animation(const std::string& animation_id, bool force_transition = false);
    std::optional<std::string> resolve_animation_by_tags(const std::vector<std::string>& required_tags,
                                                         const std::vector<std::string>& excluded_tags) const;
    std::optional<std::string> resolve_animation_by_tags_deterministic(const std::vector<std::string>& required_tags,
                                                                       const std::vector<std::string>& excluded_tags) const;
    bool set_animation_by_tags(const std::vector<std::string>& required_tags,
                               const std::vector<std::string>& excluded_tags,
                               bool force_transition = false);
    bool set_animation_by_tags_deterministic(const std::vector<std::string>& required_tags,
                                             const std::vector<std::string>& excluded_tags,
                                             bool force_transition = false);
    void begin_reverse_current_animation_until_stop();
    void begin_reverse_current_animation_to_default();
    void stop_reverse_current_animation();

    const Plan* current_plan() const { return &plan_; }
    const Plan3D* current_plan_3d() const { return &plan3d_; }
    ActivePlanMode current_plan_mode() const { return active_plan_mode_; }

    void cancel_all_movement();
    void stop_movement();

private:

    bool has_pending_move() const { return move_pending_; }
    bool has_pending_move_3d() const { return move_pending_3d_; }
    struct MoveRequest {
        SDL_Point    delta{0, 0};
        std::string  animation_id;
        bool         resort_z = true;
        bool         override_non_locked = true;
        ReversePlaybackCommand reverse_command = ReversePlaybackCommand::None;
};
    struct MoveRequest3D {
        axis::WorldPos delta{0, 0, 0};
        std::string    animation_id;
        bool           resort_z = true;
        bool           override_non_locked = true;
    };

    MoveRequest consume_move_request();
    MoveRequest3D consume_move_request_3d();
    bool consume_input_event();

private:
    friend class AnimationRuntime;
    friend class Asset;

    void set_runtime(AnimationRuntime* runtime) { runtime_ = runtime; }

    void clear_movement_plan();
    std::size_t path_index_for(const std::string& anim_id) const;
    SDL_Point final_dest{0, 0};
    axis::WorldPos final_dest_3d{0, 0, 0};

    Asset*  self_          = nullptr;
    Assets* assets_owner_  = nullptr;
    vibble::grid::Grid* grid_service_ = nullptr;
    AnimationRuntime* runtime_ = nullptr;

    Plan   plan_{};
    Plan3D plan3d_{};
    int    visited_thresh_ = 0;

    PathSanitizer   sanitizer_{};
    GetBestPath     planner_{};
    PathSanitizer3D sanitizer_3d_{};
    GetBestPath3D   planner_3d_{};
    ActivePlanMode  active_plan_mode_ = ActivePlanMode::None;
    bool            auto_move_attacking_enabled_ = false;

    bool         input_event_ = false;
    bool         move_pending_ = false;
    MoveRequest  pending_move_{};
    bool         move_pending_3d_ = false;
    MoveRequest3D pending_move_3d_{};
    ReversePlaybackCommand pending_reverse_command_ = ReversePlaybackCommand::None;
    bool        debug_enabled_ = false;
    std::optional<std::string> pending_engagement_target_asset_id_ = std::nullopt;
    std::uint32_t next_plan_retry_frame_ = 0;
    std::uint32_t local_plan_frame_counter_ = 0;
    std::uint32_t plan_variance_attempt_counter_ = 0;
    static constexpr std::uint32_t kPlanRetryCooldownFrames = 4;

    std::uint32_t resolve_plan_frame_id();
    bool planning_retry_cooldown_active(std::uint32_t frame_id) const;
    void arm_plan_retry_cooldown(std::uint32_t frame_id);
    void clear_plan_retry_cooldown();

    vibble::grid::Grid& grid() const;
    int effective_grid_resolution(std::optional<int> override_resolution) const;
    AutoMoveCombatOptions resolve_auto_move_combat_options(AutoMoveCombatOverrides overrides = {}) const;
    bool should_defer_auto_move_for_committed_attack() const;
    MovementTagFilter resolve_movement_tag_filter(const AutoMoveCombatOverrides& overrides) const;
};
