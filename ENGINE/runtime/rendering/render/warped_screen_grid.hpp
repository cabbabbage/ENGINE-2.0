#pragma once

#include "rendering/render/camera_controller.hpp"
#include "rendering/render/image_effect_settings.hpp"
#include "utils/area.hpp"
#include <SDL.h>
#include <cstdint>
#include <algorithm>
#include <optional>
#include <vector>
#include <unordered_map>
#include <memory>
#include <nlohmann/json.hpp>

struct Vec3 {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

struct CameraState {
    bool   valid = false;
    Vec3   position{};
    Vec3   forward{};
    Vec3   right{};
    Vec3   up{};
    SDL_FPoint anchor_world_px{0.0f, 0.0f};
    double camera_height = 0.0;
    double focus_depth   = 0.0;
    double reference_depth = 1.0;
    double tan_half_fov_y = 0.0;
    double tan_half_fov_x = 0.0;
    double near_plane = 0.0;
    double far_plane  = 0.0;
    double horizon_screen_y = 0.0;
    double meters_scale = 1.0;
    double pitch_radians = 0.0;
    float  pitch_degrees = 0.0f;
    double camera_world_y = 0.0;
    double anchor_world_y = 0.0;
    double focus_ndc_offset = 0.0;
    double screen_zoom = 1.0;
    double inv_screen_zoom = 1.0;
    double screen_pan_y_px = 0.0;
    float  near_camera_max_perspective_scale = 0.0f;
    float  offscreen_fade_amount_px = 0.0f;
};

class Asset;
class Room;
class CurrentRoomFinder;
namespace world {
    class WorldGrid;
    struct GridPoint;
    struct Chunk;
}

class WarpedScreenGrid {
public:

    static constexpr float kMinHeightAnchors = 0.5f;
    static constexpr float kMaxHeightAnchors = 20.0f;
    static constexpr float kMinPitchDegrees = 0.0f;
    static constexpr float kMaxPitchDegrees = 150.0f;

    struct RealismSettings {

        float min_visible_screen_ratio     = 0.015f;



        float base_height_px               = 1000.0f;

        int   render_quality_percent       = 100;

        float meters_per_100_world_px         = 1.0f;

        float extra_cull_margin = 1000.0f;
        float depth_near_world = 0.0f;
        float depth_far_world  = 5000.0f;
        float near_camera_max_perspective_scale = 4.0f;
        float offscreen_fade_amount_px = 200.0f;
};

    struct FloorDepthParams {
        bool enabled = false;
        double horizon_screen_y = 0.0;
        double bottom_screen_y = 0.0;
        double camera_height = 0.0;
        double focus_depth = 0.0;
        double pitch_radians = 0.0;
        double anchor_world_y = 0.0;
        double base_world_y = 0.0;
        double camera_world_y = 0.0;
        double focus_ndc_offset = 0.0;
        double horizon_ndc = 0.0;
        double near_ndc = -1.0;
        double ndc_scale = 1.0;
        double pitch_norm = 0.0;
        double strength = 0.0;
};

    struct RenderEffects {
        SDL_FPoint screen_position{0.0f, 0.0f};
        float vertical_scale = 1.0f;
        float distance_scale = 1.0f;
        float horizon_fade_alpha = 1.0f;
};

    struct GridBounds {
        float left = 0.0f;
        float top = 0.0f;
        float right = 0.0f;
        float bottom = 0.0f;
};

    struct RenderSmoothingKey {
        std::uint64_t asset_id = 0;
        int frame_index = 0;

        RenderSmoothingKey() = default;
        RenderSmoothingKey(std::uint64_t id, int frame) : asset_id(id), frame_index(frame) {}
        explicit RenderSmoothingKey(const Asset* asset, int frame = 0);
};

    WarpedScreenGrid(int screen_width, int screen_height, const Area& starting_view);
    ~WarpedScreenGrid();


    void update_camera_height(Room* cur, CurrentRoomFinder* finder, Asset* player, bool refresh_requested, float dt, bool dev_mode = false);
    void animate_height_to_scale(double target_height_px, int steps = 10);

    void set_focus_override(SDL_Point focus);
    void clear_focus_override();
    bool has_focus_override() const { return camera_.has_focus_override(); }
    SDL_Point get_focus_override_point() const { return camera_.state().focus_override; }

    void set_realism_settings(const RealismSettings& settings);
    void set_screen_center(SDL_Point p, bool snap_immediately = true);
    void set_up_rooms(CurrentRoomFinder* finder);
    void apply_camera_settings(const nlohmann::json& data);
    nlohmann::json camera_settings_to_json() const;

    SDL_FPoint map_to_screen(SDL_Point world) const;
    SDL_FPoint map_to_screen_f(SDL_FPoint world) const;
    bool project_world_point(SDL_FPoint world, float world_z, SDL_FPoint& out) const;
    SDL_FPoint screen_to_map(SDL_Point screen) const;

    RenderEffects compute_render_effects(SDL_Point world, float asset_screen_height, float reference_screen_height, RenderSmoothingKey smoothing_key, int world_z = 0) const;

    FloorDepthParams compute_floor_depth_params() const;
    float warp_floor_screen_y(float world_y, float linear_screen_y) const;

    double current_camera_height() const { return runtime_camera_height_; }
    double current_focus_depth() const { return runtime_focus_depth_; }
    double current_focus_ndc_offset() const { return runtime_focus_ndc_offset_; }
    float current_depth_offset_px() const { return runtime_depth_offset_px_; }
    double current_anchor_world_y() const { return runtime_anchor_world_y_; }
    double current_pitch_radians() const { return runtime_pitch_rad_; }
    float current_pitch_degrees() const { return runtime_pitch_deg_; }

    double view_height_world() const;
    double anchor_world_y() const;
    SDL_FPoint get_view_center_f() const;
    SDL_Point get_screen_center() const {
        SDL_FPoint center = camera_.state().center;
        return SDL_Point{
            static_cast<int>(center.x), static_cast<int>(center.y) };
    }
    void recompute_current_view();

    void clear_grid_state();
    void rebuild_grid_bounds();
    void rebuild_grid(world::WorldGrid& world_grid, float dt_seconds);
    void project_to_screen(world::GridPoint& point) const;
    world::GridPoint* grid_point_for_asset(const Asset* asset);
    const world::GridPoint* grid_point_for_asset(const Asset* asset) const;

    const RealismSettings& get_settings() const { return settings_; }
    RealismSettings& get_settings() { return settings_; }
    const RealismSettings& realism_settings() const { return settings_; }
    RealismSettings& realism_settings() { return settings_; }
    void set_depth_enabled(bool enabled) { depth_enabled_ = enabled; }
    bool depth_enabled() const { return depth_enabled_; }
    void set_depth_debug_logging(bool enabled) { depth_debug_logging_ = enabled; }
    bool depth_debug_logging() const { return depth_debug_logging_; }
    void set_render_areas_enabled(bool enabled) { render_areas_enabled_ = enabled; }
    const Area& get_current_view() const { return current_view_; }
    const Area& get_camera_area() const { return current_view_; }

    bool is_manual_height_override() const;
    void set_manual_height_override(bool);
    bool is_manual_zoom_override() const;
    void set_manual_zoom_override(bool);
    double get_zoom_percent() const;
    void set_zoom_percent(double percent);
    void adjust_zoom_percent(double delta_percent);
    double get_scale() const;
    void set_scale(double);
    void update();
    void set_tilt_override(std::optional<float> tilt_deg);
    void clear_tilt_override();
    std::optional<float> tilt_override() const;
    bool has_tilt_override() const { return tilt_override_deg_.has_value(); }
    float tilt_override_deg() const { return tilt_override_deg_.value_or(camera_math::kDefaultCameraTiltDeg); }
    Area frame_to_area(const SDL_Rect& frame) const;
    SDL_Point pan_and_height_to_point(double pan, double height) const;
    void animate_height_multiply(double factor);
    void animate_height_towards_point(double target_height, SDL_Point target_point);
    bool is_height_animating() const;
    SDL_Point pan_and_height_to_asset(double pan, double height, const Asset* asset) const;

    double default_camera_height_for_room(const Room* room) const;
    const std::vector<world::GridPoint*>& get_warped_points() const { return warped_points_; }
    const std::vector<Asset*>& get_visible_assets() const { return visible_assets_; }
    const std::vector<world::GridPoint*>& get_visible_points() const { return visible_points_; }
    const std::vector<world::GridPoint*>& grid_visible_points() const { return visible_points_; }
    const std::vector<world::Chunk*>& get_active_chunks() const { return active_chunks_; }
    const GridBounds& get_bounds() const { return bounds_; }
    const SDL_Rect& get_cached_world_rect() const { return cached_world_rect_; }
    std::uint32_t last_nodes_visited() const { return last_nodes_visited_; }
    std::uint32_t last_branches_skipped() const { return last_branches_skipped_; }
    int last_min_world_z() const { return last_min_world_z_; }
    int last_max_world_z() const { return last_max_world_z_; }
    std::uint32_t last_depth_culled() const { return last_depth_culled_; }
    void set_frustum_padding_world(float padding);
    float frustum_padding_world() const { return frustum_padding_world_; }
    world::GridPoint* pick_nearest_point(SDL_Point screen_pt, float max_distance_px = 32.0f);
    Area convert_area_to_aspect(const Area& in) const;
    const CameraController::State& camera_state() const { return camera_.state(); }
    std::uint64_t camera_state_version() const { return camera_state_version_; }

private:
    const CameraState& camera_state_cached() const;
    void invalidate_camera_cache();

    // --- Camera parameter state for explicit per-room camera ---


    double compute_room_scale_from_area(const Room* room) const;

    int screen_width_ = 0;
    int screen_height_ = 0;
    double aspect_ = 1.0;

    bool render_areas_enabled_ = false;
    bool lock_anchor_to_screen_center_ = false; // dev-mode option to pin anchor to screen center
    RealismSettings settings_{};

    CameraController camera_;

    Area base_view_;
    Area current_view_;

    Room* starting_room_ = nullptr;
    double starting_area_ = 0.0;

    double runtime_camera_height_ = 0.0;
    double runtime_focus_depth_ = 0.0;
    double runtime_anchor_world_y_ = 0.0;
    double runtime_focus_ndc_offset_ = 0.0;
    double runtime_pitch_rad_ = 0.0;
    float runtime_pitch_deg_ = 0.0f;
    float runtime_depth_offset_px_ = 0.0f;

    mutable std::unique_ptr<CameraState> cached_camera_state_;
    mutable bool cached_camera_state_dirty_ = true;

    std::vector<world::GridPoint*> warped_points_;
    std::vector<Asset*> visible_assets_;
    std::vector<world::GridPoint*> visible_points_;
    std::vector<world::Chunk*> active_chunks_;
    std::unordered_map<const Asset*, world::GridPoint*> asset_to_point_;
    std::uint64_t frame_counter_ = 0;
    std::uint64_t camera_state_version_ = 0;
    std::uint32_t last_nodes_visited_ = 0;
    std::uint32_t last_branches_skipped_ = 0;
    std::uint32_t last_depth_culled_ = 0;
    int last_min_world_z_ = 0;
    int last_max_world_z_ = 0;
    SDL_Rect cached_world_rect_{0, 0, 0, 0};
    GridBounds bounds_{};
    float frustum_padding_world_ = 0.0f;
    bool depth_enabled_ = true;
    bool depth_debug_logging_ = false;
    std::optional<float> tilt_override_deg_{};
};
