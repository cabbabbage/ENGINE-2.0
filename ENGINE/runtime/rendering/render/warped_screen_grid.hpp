#pragma once

#include "rendering/render/camera_controller.hpp"
#include "rendering/render/projected_sprite_frame.hpp"
#include "utils/area.hpp"
#include <SDL3/SDL.h>
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
    double min_perspective_depth = 0.0;
    float max_perspective_scale = 1.0f;
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
    // עוגנים קנוניים: הגובה נפרד מעומק מישור הקרקע.
    double anchor_world_z = 0.0; // עוגן עומק (Z) על מישור הקרקע
};

class Asset;
class Room;
class CurrentRoomFinder;
namespace world {
    class WorldGrid;
    struct GridPoint;
    struct Chunk;
    struct CameraProjectionParams;
}

class WarpedScreenGrid {
public:
    enum class CameraTransitionState {
        Idle = 0,
        BlendingToNewRoom = 1,
        Settling = 2
    };

    static constexpr float kMinHeightAnchors = 0.5f;
    static constexpr float kMaxHeightAnchors = 20.0f;
    static constexpr float kMinPitchDegrees = camera_math::kMinSupportedCameraTiltDeg;
    static constexpr float kMaxPitchDegrees = camera_math::kMaxSupportedCameraTiltDeg;

    struct RealismSettings {

        float min_visible_screen_ratio     = 0.003f;
        float boundary_min_visible_screen_ratio = 0.015f;
        bool min_visible_uses_light_radius = true;
        float base_height_px               = 1000.0f;
        float max_cull_depth               = 8000.0f;
        float light_max_cull_depth         = 32000.0f;
        float dynamic_renderer_depth_efficiency_depth = 2000.0f;
        float dynamic_renderer_depth_efficiency_min_density_ratio = 0.10f;
        float near_light_depth_threshold = 250.0f;
        float mid_light_depth_threshold  = 1200.0f;
        float far_light_depth_threshold  = 4000.0f;
        float near_light_cap = 12.0f;
        float mid_light_cap  = 24.0f;
        float far_light_cap  = 32.0f;
        float shadow_quality_budget = 1.0f;
        float global_ambient = 0.08f;
        float global_exposure = 1.0f;
        float layer_depth_interval         = 250.0f;
        float layer_depth_curve            = 1.0f;
        bool light_radius_overlap_culling_enabled = true;
        bool light_fade_smoothing_enabled = true;
        float light_fade_in_seconds = 0.16f;
        float light_fade_out_seconds = 0.28f;
        float light_min_fade_seconds = 0.06f;
        float light_distance_fade_start_ratio = 0.8f;
        bool light_culling_debug_overlay = false;
        float blur_px                      = 12.0f;
        float radial_blur_px               = 48.0f;
        bool depth_of_field_enabled         = false;
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

    struct CameraTransitionSettings {
    // דעיכה יציבה מסדר ראשון ביחס ל-dt (1/שניות). ערכים גדולים מתייצבים מהר יותר.
        float transition_damping = 9.0f;
    // תקרת מהירות בפיקסלי עולם לתנועת מרכז המצלמה.
        float max_camera_velocity = 2200.0f;
    // סקייל שמוחל על דעיכה בזמן מיזוג בין חדרים (<1 מאט מעברים).
        float room_blend_damping_scale = 0.14f;
    // סקייל שמוחל על מהירות המצלמה המקסימלית בזמן מיזוג בין חדרים.
        float room_blend_velocity_scale = 0.18f;
    // סקייל שמוחל על השפעת מעקב השחקן בזמן מיזוג בין חדרים.
        float room_blend_follow_weight_scale = 0.28f;
    // המשך התייצבות קצרה לעבר יעד החדר אחרי שהתנועה נעצרת.
        float settle_duration_after_stop = 0.20f;
    // סקייל אופציונלי לחיזוי תנועה קדימה. ערך 0 מבטל חיזוי.
        float movement_look_ahead_weight = 0.12f;
    // ערבב את יעד החדר לעבר פוקוס השחקן (0 שומר על מסגור החדר, 1 ננעל לשחקן).
        float player_follow_weight = 0.75f;
    // רדיוס רצועה רכה ממרכז המצלמה לפוקוס השחקן, בפיקסלי עולם.
        float player_soft_leash_px = 220.0f;
    // רדיוס רצועה קשיחה שפוקוס השחקן לא אמור לעבור.
        float player_hard_leash_px = 360.0f;
    };

    struct CameraTransitionTelemetry {
        CameraTransitionState state = CameraTransitionState::Idle;
        SDL_FPoint target{0.0f, 0.0f};
        SDL_FPoint velocity{0.0f, 0.0f};
        float blend_factor = 0.0f;
        float settle_time_remaining = 0.0f;
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

    struct VisibleTraversalEntry {
        Asset* asset = nullptr;
        world::GridPoint* grid_point = nullptr;
        double depth_from_anchor = 0.0;
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
    void set_screen_dimensions(int screen_width, int screen_height);
    void set_up_rooms(CurrentRoomFinder* finder);
    void apply_camera_settings(const nlohmann::json& data);
    nlohmann::json camera_settings_to_json() const;

    // מקרין למרחב המסך נקודה ששוכנת על מישור הקרקע (x/z).
    SDL_FPoint map_to_screen(SDL_Point world) const;
    SDL_FPoint map_to_screen_f(SDL_FPoint world) const;
    // world.y הוא גובה (Y).
    // world_z נושא עומק (Z) ביחס לעוגן המצלמה.
    bool project_world_point(SDL_FPoint world, float world_z, SDL_FPoint& out) const;
    bool sample_perspective_scale(SDL_FPoint world, float world_z, float& out_scale) const;
    bool build_camera_ray_from_screen(const SDL_FPoint& screen_point,
                                      render_projection::CameraRay& out_ray) const;
    bool screen_to_world_on_depth_plane(const SDL_FPoint& screen_point,
                                        float world_z,
                                        render_projection::WorldPoint3& out_world_point) const;
    SDL_FPoint screen_to_map(SDL_Point screen) const;

    // world.x ממופה ל-X ו-world.y לגובה (Y).
    // world_z מקודד עומק (Z).
    RenderEffects compute_render_effects(SDL_Point world, float asset_screen_height, float reference_screen_height, RenderSmoothingKey smoothing_key, int world_z = 0) const;

    FloorDepthParams compute_floor_depth_params() const;
    float warp_floor_screen_y(float world_y, float linear_screen_y) const;

    double current_camera_height() const { return runtime_camera_height_; }
    double current_focus_depth() const { return runtime_focus_depth_; }
    double current_focus_ndc_offset() const { return runtime_focus_ndc_offset_; }
    float current_depth_offset_px() const { return runtime_depth_offset_px_; }
    double current_focus_plane_world_z() const { return runtime_anchor_world_z_; }
    double current_anchor_world_z() const { return current_focus_plane_world_z(); }
    double current_pitch_radians() const { return runtime_pitch_rad_; }
    float current_pitch_degrees() const { return runtime_pitch_deg_; }

    double view_height_world() const;
    double focus_plane_world_z() const;
    double anchor_world_z() const;
    SDL_FPoint get_view_center_f() const;
    SDL_Point get_screen_center() const {
        SDL_FPoint center = camera_.state().center;
        return SDL_Point{
            static_cast<int>(center.x), static_cast<int>(center.y) };
    }
    void recompute_current_view();

    void clear_grid_state();
    void rebuild_grid_bounds();
    void rebuild_grid(world::WorldGrid& world_grid,
                      float dt_seconds,
                      std::uint64_t frame_id);
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
    const Area& get_display_area() const { return display_view_; }
    world::CameraProjectionParams projection_params() const;

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
    const CameraTransitionSettings& transition_settings() const { return transition_settings_; }
    const CameraTransitionTelemetry& camera_transition_telemetry() const { return transition_telemetry_; }
    static const char* transition_state_name(CameraTransitionState state);
    std::uint64_t camera_state_version() const;
    std::uint64_t projection_state_version() const { return camera_state_version(); }
    const std::vector<VisibleTraversalEntry>& visible_traversal_entries() const { return visible_traversal_entries_; }
    SDL_FPoint WarpedScreenGrid::getAnchorPoint();
    
private:
    struct ProjectionFingerprint {
        std::int64_t center_x_q = 0;
        std::int64_t center_y_q = 0;
        std::int64_t height_px_q = 0;
        std::int64_t tilt_deg_q = 0;
        std::int64_t zoom_percent_q = 0;
        std::int64_t pan_y_px_q = 0;
        std::int64_t aspect_q = 0;
        int screen_width = 0;
        int screen_height = 0;
        bool lock_anchor_to_center = false;
        bool depth_enabled = true;
        bool has_tilt_override = false;
        std::int64_t tilt_override_q = 0;

        bool operator==(const ProjectionFingerprint& other) const noexcept {
            return center_x_q == other.center_x_q &&
                   center_y_q == other.center_y_q &&
                   height_px_q == other.height_px_q &&
                   tilt_deg_q == other.tilt_deg_q &&
                   zoom_percent_q == other.zoom_percent_q &&
                   pan_y_px_q == other.pan_y_px_q &&
                   aspect_q == other.aspect_q &&
                   screen_width == other.screen_width &&
                   screen_height == other.screen_height &&
                   lock_anchor_to_center == other.lock_anchor_to_center &&
                   depth_enabled == other.depth_enabled &&
                   has_tilt_override == other.has_tilt_override &&
                   tilt_override_q == other.tilt_override_q;
        }
    };

    const CameraState& camera_state_cached() const;
    void invalidate_camera_cache();

    // --- Camera parameter state for explicit per-room camera ---


    double compute_room_scale_from_area(const Room* room) const;

    int screen_width_ = 0;
    int screen_height_ = 0;
    double aspect_ = 1.0;

    bool render_areas_enabled_ = false;
    bool lock_anchor_to_screen_center_ = false; // נשאר לא נעול כדי להימנע מעיוות סקייל הפרספקטיבה
    RealismSettings settings_{};

    CameraController camera_;

    Area base_view_;
    Area current_view_;
    Area display_view_;

    Room* starting_room_ = nullptr;
    double starting_area_ = 0.0;

    double runtime_camera_height_ = 0.0;
    double runtime_focus_depth_ = 0.0;
    double runtime_anchor_world_z_ = 0.0;
    double runtime_focus_ndc_offset_ = 0.0;
    double runtime_pitch_rad_ = 0.0;
    float runtime_pitch_deg_ = 0.0f;
    float runtime_depth_offset_px_ = 0.0f;

    mutable std::unique_ptr<CameraState> cached_camera_state_;
    mutable bool cached_camera_state_dirty_ = true;
    mutable std::optional<ProjectionFingerprint> last_projection_fingerprint_{};

    std::vector<world::GridPoint*> warped_points_;
    std::vector<VisibleTraversalEntry> visible_traversal_entries_;
    std::unordered_map<const Asset*, world::GridPoint*> asset_to_point_;
    std::unordered_map<const Asset*, std::uint8_t> visibility_reason_flags_;
    std::uint64_t frame_counter_ = 0;
    mutable std::uint64_t camera_state_version_ = 0;
    std::uint64_t last_projection_cache_invalidation_version_ = 0;
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
    const Asset* tracked_player_asset_ = nullptr;
    CameraTransitionSettings transition_settings_{};
    CameraTransitionTelemetry transition_telemetry_{};
    Room* previous_transition_room_ = nullptr;
    SDL_FPoint previous_player_world_{0.0f, 0.0f};
    bool previous_player_world_valid_ = false;
    bool previous_player_moving_ = false;
    float settle_time_remaining_ = 0.0f;
};
