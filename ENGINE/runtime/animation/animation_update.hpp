#pragma once

#include <optional>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

#include "stride_types.hpp"
#include "path_sanitizer.hpp"
#include "get_best_path.hpp"
#include "rendering/render/warped_screen_grid.hpp"
#include "assets/animation.hpp"
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

struct AnimationPlayer {
    Animation* m_animation = nullptr;
    int m_start_frame = 0;
    int m_end_frame = 0;
    int m_current_frame = 0;
    int m_variant = 0;

    SDL_Texture* current_texture() const {
        if (!m_animation || m_current_frame < 0 || m_current_frame >= int(m_animation->frames.size())) {
            return nullptr;
        }
        const auto* frame = m_animation->frames[m_current_frame];
        if (!frame || frame->variants.empty()) {
            return nullptr;
        }
        int variant_index = m_variant;
        if (variant_index < 0 || variant_index >= static_cast<int>(frame->variants.size())) {
            variant_index = 0;
        }
        return frame->variants[variant_index].base_texture;
    }
};

namespace animation_update::detail {

inline constexpr const char kDefaultAnimation[] = "default";
inline constexpr int        kOverlapDistanceSq  = 40 * 40;

bool should_consider_overlap(const Asset& self, const Asset& other);
int  distance_sq(const world::GridPoint& a, const world::GridPoint& b);
int  distance_sq(SDL_Point a, SDL_Point b);
bool segment_hits_area(const world::GridPoint& from, const world::GridPoint& to, const Area& area);
bool segment_hits_area(SDL_Point from, SDL_Point to, const Area& area);
world::GridPoint bottom_middle_for(const Asset& asset, const world::GridPoint& pos);
SDL_Point bottom_middle_for(const Asset& asset, SDL_Point pos);
SDL_Point frame_world_delta(const AnimationFrame& frame, const Asset&          asset, const vibble::grid::Grid& grid);
bool bottom_point_inside_playable_area(const Assets* assets, const world::GridPoint& bottom_point);
bool bottom_point_inside_playable_area(const Assets* assets, SDL_Point bottom_point);
bool segment_leaves_playable_area(const Assets* assets, const world::GridPoint& from, const world::GridPoint& to);
bool segment_leaves_playable_area(const Assets* assets, SDL_Point from, SDL_Point to);

}

class AnimationUpdate {
public:
    AnimationUpdate(Asset* self, Assets* assets);
    void set_debug_enabled(bool enabled);
    bool debug_enabled() const;

    void auto_move(const std::vector<SDL_Point>& rel_checkpoints, int visited_thresh_px, std::optional<int> checkpoint_resolution = std::nullopt, bool override_non_locked = true);
    void auto_move(SDL_Point world_checkpoint, int visited_thresh_px = 0, std::optional<int> checkpoint_resolution = std::nullopt, bool override_non_locked = true);
    void auto_move(Asset* target_asset, int visited_thresh_px = 0, bool override_non_locked = true);

    int visit_threshold_px() const { return visited_thresh_; }

    void move(SDL_Point delta, const std::string& animation, bool               resort_z            = true, bool               override_non_locked = true);

    void set_animation(const std::string& animation_id);

    const Plan* current_plan() const { return &plan_; }

    void cancel_all_movement();

private:

    bool has_pending_move() const { return move_pending_; }
    struct MoveRequest {
        SDL_Point    delta{0, 0};
        std::string  animation_id;
        bool         resort_z = true;
        bool         override_non_locked = true;
};
    MoveRequest consume_move_request();
    bool consume_input_event();

private:
    friend class AnimationRuntime;
    friend class Asset;

    void set_runtime(AnimationRuntime* runtime) { runtime_ = runtime; }

    void clear_movement_plan();
    std::size_t path_index_for(const std::string& anim_id) const;
    AnimationPlayer& player() { return player_; }
    SDL_Point final_dest{0, 0};

    AnimationPlayer player_{};

    Asset*  self_          = nullptr;
    Assets* assets_owner_  = nullptr;
    vibble::grid::Grid* grid_service_ = nullptr;
    AnimationRuntime* runtime_ = nullptr;

    Plan plan_{};
    int  visited_thresh_ = 0;

    PathSanitizer sanitizer_{};
    GetBestPath   planner_{};

    bool        input_event_ = false;
    bool        move_pending_ = false;
    MoveRequest pending_move_{};
    bool        debug_enabled_ = false;

    vibble::grid::Grid& grid() const;
    int effective_grid_resolution(std::optional<int> override_resolution) const;
};
