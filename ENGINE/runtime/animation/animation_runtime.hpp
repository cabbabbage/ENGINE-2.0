#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <SDL3/SDL.h>

#include "assets/asset/Asset.hpp"
#include "stride_types.hpp"
#include "path_sanitizer.hpp"
#include "path_sanitizer_3d.hpp"
#include "get_best_path.hpp"
#include "get_best_path_3d.hpp"
#include "movement_plan_executor.hpp"
#include "gameplay/world/grid_point.hpp"

namespace vibble::grid {
class Grid;
}

class Asset;
class Assets;
class AnimationFrame;
class Animation;
class AnimationUpdate;

class PathSanitizer;
class GetBestPath;
class MovementPlanExecutor;

class AnimationRuntime {
public:
    enum class ReversePlaybackMode {
        None,
        ReverseUntilStopCurrentAnimation,
        ReverseToDefaultAtStart,
    };

    AnimationRuntime(Asset* self, Assets* assets);

    void update();

    void set_planner(AnimationUpdate* planner);

    std::size_t path_index_for(const std::string& anim_id) const;

    vibble::grid::Grid& grid() const;
    bool path_blocked(const world::GridPoint& from, const world::GridPoint& to, const Asset* ignored, std::vector<const Asset*>* blockers = nullptr) const;
    bool path_blocked(SDL_Point from, SDL_Point to, const Asset* ignored, std::vector<const Asset*>* blockers = nullptr) const;
    bool handle_blocked_path(const world::GridPoint& from, const world::GridPoint& to, const std::vector<const Asset*>& blockers);
    bool handle_blocked_path(SDL_Point from, SDL_Point to, const std::vector<const Asset*>& blockers);
    void mark_progress_toward_checkpoints();
    void mark_progress_toward_checkpoints_3d();
    bool advance(AnimationFrame*& frame);
    void switch_to(const std::string& anim_id, std::size_t path_index = 0);
    bool should_defer_for_non_locked(bool override_non_locked) const;
    void begin_reverse_current_animation_until_stop();
    void begin_reverse_current_animation_to_default();
    void stop_reverse_current_animation();
    ReversePlaybackMode reverse_playback_mode() const { return reverse_playback_mode_; }

    void reset_plan_progress();
    void set_debug_enabled(bool enabled);

    bool has_active_plan() const;
    bool maybe_trigger_attack_on_cycle_boundary();

private:
    int        effective_grid_resolution(std::optional<int> override_resolution) const;
    SDL_Point  convert_delta_to_world(SDL_Point delta, int resolution) const;
    world::GridPoint bottom_middle(const world::GridPoint& pos) const;
    SDL_Point  bottom_middle(SDL_Point pos) const;
    bool       point_in_impassable(const world::GridPoint& pt, const Asset* ignored) const;
    bool       point_in_impassable(SDL_Point pt, const Asset* ignored) const;
    bool       attempt_unstick(const world::GridPoint& from, const world::GridPoint& to, const std::vector<const Asset*>& blockers);
    bool       attempt_unstick(SDL_Point from, SDL_Point to, const std::vector<const Asset*>& blockers);
    bool       adjust_next_checkpoint(const std::vector<const Asset*>& blockers);
    bool       adjust_next_checkpoint_3d(const std::vector<const Asset*>& blockers);
    bool       replan_to_destination();
    bool       replan_to_destination_3d();
    bool       consume_replan_attempt_budget();
    float      parent_world_z() const;

    void       apply_pending_move();
    void       apply_pending_move_3d();
    void       clear_reverse_playback_state();
    void       activate_reverse_playback(ReversePlaybackMode mode);
    AnimationFrame* last_frame_for(const Animation& anim, std::size_t path_index) const;
    bool       reverse_mode_applies_to_current_animation() const;
    bool       attacking_enabled_for_self() const;
    std::vector<std::string> attack_animation_candidates() const;
    std::vector<Asset*> attack_candidate_targets() const;
    std::uint32_t resolve_frame_id_for_cooldown();

private:
    friend class MovementPlanExecutor;

    Asset*  self_         = nullptr;
    Assets* assets_owner_ = nullptr;
    vibble::grid::Grid* grid_service_ = nullptr;
    AnimationUpdate* planner_iface_ = nullptr;

    std::size_t stride_index_         = 0;
    int         stride_frame_counter_ = 0;
    std::size_t next_checkpoint_index_ = 0;

    PathSanitizer  sanitizer_{};
    GetBestPath    planner_{};
    PathSanitizer3D sanitizer_3d_{};
    GetBestPath3D   planner_3d_{};
    MovementPlanExecutor   executor_{};

    std::unordered_map<std::string, std::size_t> active_paths_{};

    bool debug_enabled_ = false;
    ReversePlaybackMode reverse_playback_mode_ = ReversePlaybackMode::None;
    std::string reverse_playback_animation_id_{};
    bool lock_on_end_active_ = false;
    int  suppress_root_motion_frames_ = 0;
    std::uint32_t replan_budget_frame_id_ = 0;
    int replan_attempts_this_frame_ = 0;
    static constexpr int kMaxReplanAttemptsPerFrame = 3;
    std::uint32_t local_runtime_frame_id_ = 0;
    std::uint32_t next_attack_cycle_eval_frame_ = 0;
    static constexpr std::uint32_t kAttackCycleDebounceFrames = 8;

    bool suppress_root_motion_active() const { return suppress_root_motion_frames_ > 0; }
};
