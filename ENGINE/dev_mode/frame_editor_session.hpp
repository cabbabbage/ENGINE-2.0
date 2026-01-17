#pragma once

#include <SDL.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include <nlohmann/json.hpp>

#include "dev_mode/pan_and_height.hpp"
#include "animation_update/combat_geometry.hpp"

#ifndef FRAME_EDITOR_ACCESS
#define FRAME_EDITOR_ACCESS private
#endif

class Assets;
class Asset;
class AssetInfo;
class Animation;
struct AnimationFrame;
class Input;
struct SDL_Renderer;
class DMButton;
class DMDropdown;
class DMNumericStepper;
class WarpedScreenGrid;
struct AnimationChildFrameData;
enum class AnimationChildMode;

namespace animation_editor {
class AnimationDocument;
class PreviewProvider;
}



class FrameEditorSession {
public:
    enum class Mode { Movement, StaticChildren, AsyncChildren, AttackGeometry, HitGeometry };
    enum class EditPlane { XZ, XY };
    enum class AdjustmentAxis { X, Y, Z };
    enum class AdjustmentTarget { None, MovementPoint, ChildPoint, AttackStart, AttackControl, AttackEnd, HitboxCenter };
    struct AdjustmentSelection {
        AdjustmentTarget target = AdjustmentTarget::None;
        AdjustmentAxis axis = AdjustmentAxis::X;
        int child_index = -1;
        int attack_vector_index = -1;
        SDL_FPoint world_pos{};
        SDL_Point screen_pos{};
        bool has_value() const { return target != AdjustmentTarget::None; }
        void reset() {
            target = AdjustmentTarget::None;
            axis = AdjustmentAxis::X;
            child_index = -1;
            attack_vector_index = -1;
            world_pos = SDL_FPoint{0.0f, 0.0f};
            screen_pos = SDL_Point{0, 0};
        }
    };
    FrameEditorSession();
    ~FrameEditorSession();

    void begin(Assets* assets,
               Asset* asset,
               std::shared_ptr<animation_editor::AnimationDocument> document,
               std::shared_ptr<animation_editor::PreviewProvider> preview,
               const std::string& animation_id,
               std::function<void(const std::string&)> on_host_closed,
               std::function<void()> on_end_callback = {});
    void end();

    bool is_active() const { return active_; }
    Mode mode() const { return mode_; }
    EditPlane edit_plane() const {
        return (mode_ == Mode::Movement) ? EditPlane::XZ : EditPlane::XY;
    }

    void update(const Input& input);
    bool handle_event(const SDL_Event& e);
    void render(SDL_Renderer* renderer) const;

    void set_snap_resolution(int r);
    bool should_render_asset(const Asset* asset) const;

FRAME_EDITOR_ACCESS:
    bool target_is_alive() const;
    void capture_camera_state();
    void lock_camera_state();
    void restore_camera_state();
    void enforce_camera_locks(WarpedScreenGrid& cam);

    struct ChildFrame {
        int child_index = -1;
        float dx = 0.0f;
        float dy = 0.0f;
        float dz = 0.0f;
        float degree = 0.0f;
        bool visible = true;
        bool has_data = false;
};
    struct MovementFrame {
        float dx = 0.0f;
        float dy = 0.0f;
        float dz = 0.0f;
        bool resort_z = false;
        std::vector<ChildFrame> children;

        animation_update::FrameHitGeometry    hit;
        animation_update::FrameAttackGeometry attack;
};

    Assets* assets_ = nullptr;
    Asset* target_ = nullptr;
    std::shared_ptr<animation_editor::AnimationDocument> document_;
    std::shared_ptr<animation_editor::PreviewProvider> preview_;
    std::function<void(const std::string&)> on_host_closed_{};
    std::function<void()> on_end_{};

    bool active_ = false;
    std::string animation_id_;
    std::vector<std::string> edited_animation_ids_;
    int selected_index_ = 0;
    Mode mode_ = Mode::Movement;
    bool show_animation_ = true;
    bool show_child_ = true;
    bool last_applied_show_asset_state_ = true;
    bool smooth_enabled_ = false;
    bool curve_enabled_ = false;
    int selected_child_index_ = 0;
    bool adjustment_mode_active_ = false;
    bool adjustment_dirty_ = false;
    AdjustmentSelection adjustment_selection_;

    bool prev_realism_enabled_ = true;
    bool prev_parallax_enabled_ = true;
    bool prev_asset_hidden_ = false;

    int  snap_resolution_r_ = 0;
    bool snap_resolution_override_ = false;

    std::vector<MovementFrame> frames_;
    std::vector<SDL_FPoint> rel_positions_;
    std::vector<float> rel_z_positions_;

    mutable std::unique_ptr<DMButton> btn_back_;
    mutable std::unique_ptr<DMButton> btn_movement_;
    mutable std::unique_ptr<DMButton> btn_children_;
    mutable std::unique_ptr<DMButton> btn_attack_geometry_;
    mutable std::unique_ptr<DMButton> btn_hit_geometry_;
    mutable std::unique_ptr<DMButton> btn_prev_;
    mutable std::unique_ptr<DMButton> btn_next_;
    mutable std::unique_ptr<DMDropdown> dd_animation_select_;
    mutable std::unique_ptr<DMNumericStepper> stepper_grid_resolution_;

    mutable std::unique_ptr<class DMButton> btn_apply_all_movement_;
    mutable std::unique_ptr<class DMButton> btn_apply_all_animations_movement_;
    mutable std::unique_ptr<class DMButton> btn_apply_all_children_;
    mutable std::unique_ptr<class DMButton> btn_apply_all_animations_children_;
    mutable std::unique_ptr<class DMButton> btn_apply_all_hit_;
    mutable std::unique_ptr<class DMButton> btn_apply_all_animations_hit_;
    mutable std::unique_ptr<class DMButton> btn_apply_all_attack_;
    mutable std::unique_ptr<class DMButton> btn_apply_all_animations_attack_;
    mutable std::unique_ptr<class DMCheckbox> cb_smooth_;
    mutable std::unique_ptr<class DMCheckbox> cb_curve_;
    mutable std::unique_ptr<class DMCheckbox> cb_show_anim_;
    mutable std::unique_ptr<class DMCheckbox> cb_show_child_;
    mutable std::unique_ptr<class DMDropdown> dd_child_select_;
    mutable std::unique_ptr<class DMDropdown> dd_child_mode_;
    mutable std::unique_ptr<class DMTextBox> tb_child_name_;
    mutable std::unique_ptr<class DMButton> btn_child_add_;
    mutable std::unique_ptr<class DMButton> btn_child_remove_;
    mutable std::unique_ptr<class DMTextBox> tb_child_dx_;
    mutable std::unique_ptr<class DMTextBox> tb_child_dy_;
    mutable std::unique_ptr<class DMTextBox> tb_child_dz_;
    mutable std::unique_ptr<class DMTextBox> tb_child_deg_;
    mutable std::unique_ptr<class DMCheckbox> cb_child_visible_;

    mutable std::unique_ptr<class DMButton> btn_hitbox_add_remove_;
    mutable std::unique_ptr<class DMButton> btn_hitbox_copy_next_;
    mutable std::unique_ptr<class DMTextBox> tb_hit_center_x_;
    mutable std::unique_ptr<class DMTextBox> tb_hit_center_z_;
    mutable std::unique_ptr<class DMTextBox> tb_hit_width_;
    mutable std::unique_ptr<class DMTextBox> tb_hit_height_;
    mutable std::unique_ptr<class DMTextBox> tb_hit_rotation_;

    mutable std::unique_ptr<class DMButton> btn_attack_add_remove_;
    mutable std::unique_ptr<class DMButton> btn_attack_delete_;
    mutable std::unique_ptr<class DMButton> btn_attack_copy_next_;
    mutable std::unique_ptr<class DMTextBox> tb_attack_start_x_;
    mutable std::unique_ptr<class DMTextBox> tb_attack_start_y_;
    mutable std::unique_ptr<class DMTextBox> tb_attack_start_z_;
    mutable std::unique_ptr<class DMTextBox> tb_attack_control_x_;
    mutable std::unique_ptr<class DMTextBox> tb_attack_control_y_;
    mutable std::unique_ptr<class DMTextBox> tb_attack_control_z_;
    mutable std::unique_ptr<class DMTextBox> tb_attack_end_x_;
    mutable std::unique_ptr<class DMTextBox> tb_attack_end_y_;
    mutable std::unique_ptr<class DMTextBox> tb_attack_end_z_;
    mutable std::unique_ptr<class DMTextBox> tb_attack_damage_;

    mutable std::unique_ptr<class DMTextBox> tb_total_dx_;
    mutable std::unique_ptr<class DMTextBox> tb_total_dy_;
    mutable std::unique_ptr<class DMTextBox> tb_frame_dx_;
    mutable std::unique_ptr<class DMTextBox> tb_frame_dy_;
    mutable std::unique_ptr<class DMTextBox> tb_frame_dz_;

    mutable std::string last_totals_dx_text_{};
    mutable std::string last_totals_dy_text_{};
    mutable std::string last_frame_dx_text_{};
    mutable std::string last_frame_dy_text_{};
    mutable std::string last_frame_dz_text_{};
    mutable bool last_show_anim_value_ = true;
    mutable bool last_show_child_value_ = true;
    mutable std::string last_child_dx_text_{};
    mutable std::string last_child_dy_text_{};
    mutable std::string last_child_dz_text_{};
    mutable std::string last_child_deg_text_{};
    mutable std::string last_child_name_text_{};
    mutable int last_child_mode_index_ = 0;
    mutable bool last_child_visible_value_ = false;
    mutable bool cb_show_anim_targets_parent_label_ = false;
    mutable std::string last_hit_center_x_text_{};
    mutable std::string last_hit_center_z_text_{};
    mutable std::string last_hit_width_text_{};
    mutable std::string last_hit_height_text_{};
    mutable std::string last_hit_rotation_text_{};
    mutable std::string last_attack_start_x_text_{};
    mutable std::string last_attack_start_y_text_{};
    mutable std::string last_attack_start_z_text_{};
    mutable std::string last_attack_control_x_text_{};
    mutable std::string last_attack_control_y_text_{};
    mutable std::string last_attack_control_z_text_{};
    mutable std::string last_attack_end_x_text_{};
    mutable std::string last_attack_end_y_text_{};
    mutable std::string last_attack_end_z_text_{};
    mutable std::string last_attack_damage_text_{};

    mutable SDL_Rect directory_rect_{0,0,0,0};
    mutable SDL_Rect toolbox_rect_{0,0,0,0};
    mutable SDL_Rect toolbox_drag_rect_{0,0,0,0};
    mutable SDL_Rect nav_rect_{0,0,0,0};
    mutable SDL_Rect nav_drag_rect_{0,0,0,0};
    mutable std::vector<SDL_Rect> toolbox_widget_rects_;
    mutable int toolbox_scroll_offset_ = 0;
    mutable int toolbox_content_height_ = 0;
    mutable bool toolbox_scrollbar_visible_ = false;
    mutable SDL_Rect toolbox_scrollbar_track_{0,0,0,0};
    mutable SDL_Rect toolbox_scrollbar_thumb_{0,0,0,0};
    SDL_Point dir_pos_{0, 0};
    SDL_Point toolbox_pos_{0, 0};
    SDL_Point nav_pos_{0, 0};
    bool dragging_dir_ = false;
    bool dragging_toolbox_ = false;
    bool dragging_toolbox_scrollbar_ = false;
    bool dragging_nav_ = false;
    bool dragging_scrollbar_thumb_ = false;
    SDL_Point drag_offset_dir_{0, 0};
    SDL_Point drag_offset_toolbox_{0, 0};
    SDL_Point drag_offset_nav_{0, 0};
    int toolbox_scrollbar_drag_offset_ = 0;
    int scrollbar_drag_offset_x_ = 0;
    mutable int scroll_offset_ = 0;
    mutable int thumb_content_width_ = 0;
    mutable int thumb_viewport_width_ = 0;
    mutable SDL_Rect scrollbar_track_{0,0,0,0};
    mutable SDL_Rect scrollbar_thumb_{0,0,0,0};
    mutable bool scrollbar_visible_ = false;
    mutable std::vector<SDL_Rect> thumb_rects_;
    mutable std::vector<int> thumb_indices_;

    mutable class PanAndHeight pan_height_;
    int pending_scroll_delta_ = 0;
    bool hover_toolbox_ = false;
    bool hover_toolbox_drag_handle_ = false;
    bool hover_nav_drag_handle_ = false;
    struct CameraLockState {
        bool valid = false;
        bool manual_override_before = false;
        bool focus_override_before = false;
        SDL_Point focus_point_before{0, 0};
        SDL_Point screen_center_before{0, 0};
        std::optional<float> tilt_override_before{};
        double camera_y_distance_before = 0.0;
    };
    CameraLockState camera_lock_state_;
    bool camera_y_distance_locked_ = false;
    double locked_camera_y_distance_ = 0.0;
    bool tilt_locked_ = false;
    float locked_tilt_deg_ = 0.0f;
    std::vector<std::string> child_assets_;
    mutable std::vector<AnimationChildMode> child_modes_;
    std::string document_payload_cache_;
    std::string document_children_signature_;
    std::unordered_map<Asset*, bool> child_hidden_cache_;
    bool last_payload_loaded_ = false;
    mutable std::vector<std::string> animation_dropdown_options_cache_;
    mutable std::vector<std::string> child_dropdown_options_cache_;
    int selected_hitbox_index_ = -1;
    enum class HitHandle { None, Move, Left, Right, Top, Bottom, Rotate };
    HitHandle active_hitbox_handle_ = HitHandle::None;
    bool hitbox_dragging_ = false;
    SDL_Point hitbox_drag_start_mouse_{0, 0};
    SDL_FPoint hitbox_drag_grab_offset_{0.0f, 0.0f};
    animation_update::FrameHitGeometry::HitBox hitbox_drag_start_box_;
    float hitbox_drag_left_ = 0.0f;
    float hitbox_drag_right_ = 0.0f;
    float hitbox_drag_top_ = 0.0f;
    float hitbox_drag_bottom_ = 0.0f;
    bool hitbox_drag_moved_ = false;

    int selected_attack_vector_index_ = -1;
    enum class AttackHandle { None, Start, Control, End, Segment };
    AttackHandle active_attack_handle_ = AttackHandle::None;
    bool attack_dragging_ = false;
    bool attack_drag_moved_ = false;
    SDL_Point attack_drag_start_mouse_{0, 0};
    SDL_FPoint attack_drag_start_mouse_local_{0.0f, 0.0f};
    animation_update::FrameAttackGeometry::Vector attack_drag_start_vector_;

    bool pending_save_ = false;

FRAME_EDITOR_ACCESS:
    void load_animation_data(const std::string& animation_id);
    void switch_animation(const std::string& animation_id);
    void ensure_widgets() const;
    void refresh_animation_dropdown() const;
    void rebuild_layout() const;
    void render_toolbox(SDL_Renderer* renderer) const;
    void apply_current_mode_to_all_frames();
    void apply_current_mode_to_all_animations();
    void apply_frame_move_from_base(int index, SDL_FPoint desired_rel, const std::vector<SDL_FPoint>& base_rel);
    void redistribute_frames_from_middle_drag(int adjusted_index);

    void redistribute_frames_after_adjustment(int adjusted_index);
    void apply_linear_smoothing(int adjusted_index, std::vector<SDL_FPoint>& redistributed, int last_index) const;
    void apply_curved_smoothing(int adjusted_index, const std::vector<SDL_FPoint>& original, std::vector<SDL_FPoint>& redistributed, int last_index) const;
    void rebuild_rel_positions();
    void ensure_child_frames_initialized();
    void smooth_child_offsets(int child_index, int adjusted_index);
    void persist_changes(bool rebuild_animation = false);

    void persist_mode_changes(Mode mode);
    void select_frame(int index);
    void select_child(int index);
    void update_asset_preview_frame() const;
    static inline MovementFrame clamp_frame(const MovementFrame& in) {
        MovementFrame f = in;
        if (!std::isfinite(f.dx)) f.dx = 0.0f;
        if (!std::isfinite(f.dy)) f.dy = 0.0f;
        if (!std::isfinite(f.dz)) f.dz = 0.0f;
        return f;
    }
    static std::vector<MovementFrame> parse_movement_frames_json(const std::string& payload_json);
    void sync_child_frames();
    void remap_child_indices(const std::vector<int>& remap);
    ChildFrame* current_child_frame();
    const ChildFrame* current_child_frame() const;
    void refresh_child_assets_from_document();
    void rebuild_child_preview_cache();
    void sync_child_asset_visibility();
    void cache_child_hidden_states();
    void apply_child_hidden_state(bool show_children);
    class Animation* current_animation_mutable() const;
    void hydrate_frames_from_animation();
    void apply_frames_to_animation();
    int max_scroll_offset() const;
    void clamp_scroll_offset() const;
    void ensure_selected_thumb_visible();
        void ensure_child_mode_size() const;
    std::vector<int> build_child_index_remap(const std::vector<std::string>& previous, const std::vector<std::string>& next) const;
    void apply_child_list_change(const std::vector<std::string>& next_children);
    void add_or_rename_child(const std::string& name);
    void remove_selected_child();
    void set_child_mode(int child_index, AnimationChildMode mode);
    AnimationChildMode child_mode(int child_index) const;
    int child_mode_index(AnimationChildMode mode) const;
    void render_hit_geometry(SDL_Renderer* renderer) const;
    bool begin_hitbox_drag(SDL_Point mouse);
    void update_hitbox_drag(SDL_Point mouse);
    void end_hitbox_drag(bool commit);

    struct DirectoryPanelMetrics {
        int width = 0;
        int height = 0;
        int top_padding = 0;
};

    struct MovementToolboxMetrics {
        int padding = 0;
        int gap = 0;
        int width = 0;
        int height = 0;
        int drag_handle_height = 0;
        int row_height = 0;
        int smooth_checkbox_width = 0;
        int curve_checkbox_width = 0;
        int show_checkbox_width = 0;
        int totals_width = 0;
        int total_dx_height = 0;
        int total_dy_height = 0;
        int frame_field_width = 0;
        int frame_dx_height = 0;
        int frame_dy_height = 0;
        int frame_dz_height = 0;
        int frame_row_height = 0;
};
    struct ChildrenToolboxMetrics {
        int padding = 0;
        int gap = 0;
        int width = 0;
        int height = 0;
        int drag_handle_height = 0;

        int dropdown_row_height = 0;
        int mode_row_height = 0;

        int movement_row_height = 0;
        int mode_dropdown_width = 0;
        int toggle_row_height = 0;
        int form_row_height = 0;
        int textbox_width = 0;
        int name_row_height = 0;
        int name_textbox_width = 0;
        int child_action_button_width = 0;
        int child_dx_height = 0;
        int child_dy_height = 0;
        int child_rotation_height = 0;
        int child_visible_checkbox_width = 0;
        int show_parent_checkbox_width = 0;
        int show_child_checkbox_width = 0;

        int smooth_checkbox_width = 0;
        int curve_checkbox_width = 0;
        int totals_width = 0;
        int total_dx_height = 0;
        int total_dy_height = 0;
};

    DirectoryPanelMetrics build_directory_panel_metrics() const;
    MovementToolboxMetrics build_movement_toolbox_metrics() const;
    ChildrenToolboxMetrics build_children_toolbox_metrics() const;

    struct HitBoxVisual {
        SDL_FPoint center{};
        std::array<SDL_FPoint, 4> corners;
        std::array<SDL_FPoint, 4> edge_midpoints;
        SDL_FPoint rotate_handle{};
};

    animation_update::FrameHitGeometry::HitBox* current_hit_box();
    const animation_update::FrameHitGeometry::HitBox* current_hit_box() const;
    void clamp_hitbox_selection();
    animation_update::FrameHitGeometry::HitBox* ensure_hit_box();
    void delete_current_hit_box();
    void refresh_hitbox_form() const;
    void copy_hit_box_to_next_frame();
    float document_scale_factor() const;
    float asset_local_scale() const;
    SDL_Point asset_anchor_world() const;
    bool screen_to_local(SDL_Point screen, SDL_FPoint& out_local) const;
    bool build_hitbox_visual(const animation_update::FrameHitGeometry::HitBox& box, HitBoxVisual& out) const;
    animation_update::FrameAttackGeometry::Vector* current_attack_vector();
    const animation_update::FrameAttackGeometry::Vector* current_attack_vector() const;
    animation_update::FrameAttackGeometry::Vector* ensure_attack_vector();
    void delete_current_attack_vector();
    int current_attack_vector_index() const;
    void set_current_attack_vector_index(int index);
    void clamp_attack_selection();
    void refresh_attack_form() const;
    void copy_attack_vector_to_next_frame();
    void render_attack_geometry(SDL_Renderer* renderer) const;
    bool begin_attack_drag(SDL_Point mouse);
    void update_attack_drag(SDL_Point mouse);
    void end_attack_drag(bool commit);
    float attachment_scale() const;

FRAME_EDITOR_ACCESS:
    void render_directory_panel(SDL_Renderer* renderer);
    void render_navigation_panel(SDL_Renderer* renderer) const;
    void render_toolbox(SDL_Renderer* renderer);
    void render_child_guides(SDL_Renderer* renderer, const WarpedScreenGrid& cam);
    void render_hitbox_guides(SDL_Renderer* renderer, const WarpedScreenGrid& cam);
    void render_attack_guides(SDL_Renderer* renderer, const WarpedScreenGrid& cam);


    float mirrored_child_rotation(bool parent_is_flipped, float degree) const;
    void apply_child_timelines_from_payload(const nlohmann::json& payload);
    nlohmann::json build_child_timelines_payload(const nlohmann::json& existing_payload) const;
    static ChildFrame child_frame_from_timeline_sample(const nlohmann::json& sample, int child_index);
    static nlohmann::json child_frame_to_json(const ChildFrame& frame);
    static bool timeline_entry_is_static(const nlohmann::json& entry);
    AnimationChildFrameData build_child_frame_descriptor(const MovementFrame& frame, std::size_t child_index) const;
    void reset_adjustment_selection();
    void exit_adjustment_mode(bool apply_smoothing = true);
    void select_adjustment_target(AdjustmentTarget target, int child_index, SDL_FPoint world_pos, SDL_Point screen_pos);
    void cycle_adjustment_axis();
    void adjust_selection_axis(int scroll_delta, const WarpedScreenGrid& cam);
    void ensure_adjustment_auto_select(const WarpedScreenGrid& cam);
    std::optional<int> hit_test_child_marker(const SDL_Point& mouse, const WarpedScreenGrid& cam, SDL_Point parent_base, float base_adjustment) const;
    SDL_FPoint movement_point_world_position() const;
    SDL_FPoint child_world_position(int child_index, float base_adjustment, bool flipped) const;
    void apply_adjustment_smoothing();
};

inline std::vector<FrameEditorSession::MovementFrame>
FrameEditorSession::parse_movement_frames_json(const std::string& payload_json) {
    std::vector<MovementFrame> frames;
    nlohmann::json payload = nlohmann::json::parse(payload_json, nullptr, false);
    if (!payload.is_object()) {
        frames.push_back(MovementFrame{});
        return frames;
    }
    nlohmann::json movement = nlohmann::json::array();
    if (payload.contains("movement")) movement = payload["movement"];
    if (!movement.is_array() || movement.empty()) {
        frames.push_back(MovementFrame{});
        return frames;
    }

    nlohmann::json hit_geom = nlohmann::json::array();
    if (payload.contains("hit_geometry")) hit_geom = payload["hit_geometry"];
    if (!hit_geom.is_array()) hit_geom = nlohmann::json::array();

    nlohmann::json attack_geom = nlohmann::json::array();
    if (payload.contains("attack_geometry")) attack_geom = payload["attack_geometry"];
    if (!attack_geom.is_array()) attack_geom = nlohmann::json::array();

    auto read_float = [](const nlohmann::json& v, float fallback = 0.0f) -> float {
        if (v.is_number()) {
            try {
                return static_cast<float>(v.get<double>());
            } catch (...) {}
        }
        if (v.is_string()) {
            try {
                return std::stof(v.get<std::string>());
            } catch (...) {}
        }
        return fallback;
};
    auto read_int = [](const nlohmann::json& v, int fallback = 0) -> int {
        if (v.is_number_integer()) {
            try {
                return v.get<int>();
            } catch (...) {}
        } else if (v.is_number()) {
            try {
                return static_cast<int>(v.get<double>());
            } catch (...) {}
        } else if (v.is_string()) {
            try {
                return std::stoi(v.get<std::string>());
            } catch (...) {}
        }
        return fallback;
};

    constexpr std::array<const char*, 3> kLegacyDamageTypeNames = {
        "projectile", "melee", "explosion"
    };
    auto append_hit_box = [&](MovementFrame& frame,
                              const nlohmann::json& node) {
        if (node.is_null()) {
            return;
        }
        animation_update::FrameHitGeometry::HitBox box;
        if (node.is_object()) {
            box.center_x = read_float(node.value("center_x", 0.0f));
            const bool has_center_z = node.contains("center_z");
            box.center_y = has_center_z ? read_float(node.value("center_y", 0.0f)) : 0.0f;
            box.center_z = has_center_z ? read_float(node.value("center_z", 0.0f))
                                        : read_float(node.value("center_y", 0.0f));
            box.half_width = read_float(node.value("half_width", 0.0f));
            box.half_height = read_float(node.value("half_height", 0.0f));
            box.rotation_degrees = read_float(node.value("rotation", node.value("rotation_degrees", 0.0f)));
        } else if (node.is_array()) {
            const auto& arr = node;
            if (!arr.empty())          box.center_x = read_float(arr[0]);
            if (arr.size() > 1)        box.center_z = read_float(arr[1]);
            if (arr.size() > 2)        box.half_width = read_float(arr[2]);
            if (arr.size() > 3)        box.half_height = read_float(arr[3]);
            if (arr.size() > 4 && arr[4].is_number()) {
                box.rotation_degrees = read_float(arr[4]);
            } else if (arr.size() > 5 && arr[5].is_number()) {
                box.rotation_degrees = read_float(arr[5]);
            }
            if (arr.size() > 4 && arr[4].is_boolean() && !arr[4].get<bool>()) {
                return;
            }
        } else {
            return;
        }
        if (box.is_empty()) {
            return;
        }
        frame.hit.boxes.push_back(box);
};

    auto append_attack_vector = [&](MovementFrame& frame,
                                    const nlohmann::json& node) {
        if (node.is_null()) return;
        animation_update::FrameAttackGeometry::Vector vec;
        if (node.is_object()) {
            vec.start_x = read_float(node.value("start_x", 0.0f));
            const bool has_start_z = node.contains("start_z");
            vec.start_y = has_start_z ? read_float(node.value("start_y", 0.0f)) : 0.0f;
            vec.start_z = has_start_z ? read_float(node.value("start_z", 0.0f))
                                      : read_float(node.value("start_y", 0.0f));
            const bool has_control = node.contains("control_x") || node.contains("control_y") || node.contains("control_z");
            const bool has_control_z = node.contains("control_z");
            if (has_control) {
                vec.control_x = read_float(node.value("control_x", (vec.start_x)));
                vec.control_y = has_control_z ? read_float(node.value("control_y", vec.start_y)) : 0.0f;
                vec.control_z = has_control_z ? read_float(node.value("control_z", vec.start_z))
                                              : read_float(node.value("control_y", vec.start_z));
            } else {
                vec.control_x = (vec.start_x + read_float(node.value("end_x", 0.0f))) * 0.5f;
                const float fallback_end_z = read_float(node.value("end_z", read_float(node.value("end_y", 0.0f))));
                const float fallback_end_y = has_start_z ? read_float(node.value("end_y", 0.0f)) : 0.0f;
                vec.control_y = (vec.start_y + fallback_end_y) * 0.5f;
                vec.control_z = (vec.start_z + fallback_end_z) * 0.5f;
            }
            vec.end_x   = read_float(node.value("end_x", 0.0f));
            const bool has_end_z = node.contains("end_z");
            vec.end_y   = has_end_z ? read_float(node.value("end_y", 0.0f)) : 0.0f;
            vec.end_z   = has_end_z ? read_float(node.value("end_z", 0.0f))
                                    : read_float(node.value("end_y", 0.0f));
            vec.damage  = read_int(node.value("damage", 0));
        } else if (node.is_array()) {
            const auto& arr = node;
            if (!arr.empty())      vec.start_x = read_float(arr[0]);
            if (arr.size() > 1)    vec.start_z = read_float(arr[1]);
            if (arr.size() > 2)    vec.end_x   = read_float(arr[2]);
            if (arr.size() > 3)    vec.end_z   = read_float(arr[3]);
            vec.control_x = (vec.start_x + vec.end_x) * 0.5f;
            vec.control_y = 0.0f;
            vec.control_z = (vec.start_z + vec.end_z) * 0.5f;
            if (arr.size() > 4)    vec.damage  = read_int(arr[4]);
        } else {
            return;
        }
        frame.attack.add_vector(vec);
};

    std::size_t frame_index = 0;
    for (const auto& entry : movement) {
        MovementFrame f{};
        if (entry.is_array()) {
            if (!entry.empty() && entry[0].is_number()) f.dx = static_cast<float>(entry[0].get<double>());
            if (entry.size() > 1 && entry[1].is_number()) f.dy = static_cast<float>(entry[1].get<double>());
            f.dz = f.dy;
            if (entry.size() > 2 && entry[2].is_boolean()) f.resort_z = entry[2].get<bool>();

            const nlohmann::json* children_json = nullptr;
            if (entry.size() > 4 && entry[4].is_array()) {
                children_json = &entry[4];
            } else if (entry.size() > 3 && entry[3].is_array()) {
                const auto& maybe_children = entry[3];
                if (!maybe_children.empty() && maybe_children[0].is_array()) {
                    children_json = &maybe_children;
                }
            } else if (entry.size() > 2 && entry[2].is_array()) {
                const auto& maybe_children2 = entry[2];
                if (!maybe_children2.empty() && maybe_children2[0].is_array()) {
                    children_json = &maybe_children2;
                }
            }
            if (children_json) {
                for (const auto& child_entry : *children_json) {
                    if (!child_entry.is_array() || child_entry.empty()) continue;
                    ChildFrame child;
                    try { child.child_index = child_entry[0].get<int>(); } catch (...) { child.child_index = -1; }
                    if (child_entry.size() > 1 && child_entry[1].is_number()) {
                        child.dx = static_cast<float>(child_entry[1].get<double>());
                    }
                    if (child_entry.size() > 2 && child_entry[2].is_number()) {
                        child.dy = static_cast<float>(child_entry[2].get<double>());
                    }
                    if (child_entry.size() > 3 && child_entry[3].is_number()) {
                        child.dz = static_cast<float>(child_entry[3].get<double>());
                        if (child_entry.size() > 4 && child_entry[4].is_number()) {
                            child.degree = static_cast<float>(child_entry[4].get<double>());
                        }
                        if (child_entry.size() > 5) {
                            if (child_entry[5].is_boolean()) {
                                child.visible = child_entry[5].get<bool>();
                            } else if (child_entry[5].is_number_integer()) {
                                child.visible = child_entry[5].get<int>() != 0;
                            }
                        }
                    } else {
                        if (child_entry.size() > 3 && child_entry[3].is_number()) {
                            child.degree = static_cast<float>(child_entry[3].get<double>());
                        }
                        if (child_entry.size() > 4) {
                            if (child_entry[4].is_boolean()) {
                                child.visible = child_entry[4].get<bool>();
                            } else if (child_entry[4].is_number_integer()) {
                                child.visible = child_entry[4].get<int>() != 0;
                            }
                        }
                        child.dz = child.dy;
                        child.dy = 0.0f;
                    }
                    child.has_data = true;
                    f.children.push_back(child);
                }
            }
        } else if (entry.is_object()) {
            f.dx = static_cast<float>(entry.value("dx", 0.0));
            f.dy = static_cast<float>(entry.value("dy", 0.0));
            f.dz = static_cast<float>(entry.value("dz", entry.value("dy", 0.0)));
            f.resort_z = entry.value("resort_z", false);
            if (entry.contains("children") && entry["children"].is_array()) {
                for (const auto& child_entry : entry["children"]) {
                    if (!child_entry.is_object() && !child_entry.is_array()) continue;
                    ChildFrame child;
                    if (child_entry.is_object()) {
                        child.child_index = child_entry.value("child_index", -1);
                        child.dx = static_cast<float>(child_entry.value("dx", 0.0));
                        child.dy = static_cast<float>(child_entry.value("dy", 0.0));
                        child.dz = static_cast<float>(child_entry.value("dz", child_entry.value("dy", 0.0)));
                        if (!child_entry.contains("dz")) {
                            child.dy = 0.0f;
                        }
                        if (child_entry.contains("degree") && child_entry["degree"].is_number()) {
                            child.degree = static_cast<float>(child_entry["degree"].get<double>());
                        } else if (child_entry.contains("rotation") && child_entry["rotation"].is_number()) {
                            child.degree = static_cast<float>(child_entry["rotation"].get<double>());
                        } else {
                            child.degree = 0.0f;
                        }
                        child.visible = child_entry.value("visible", true);
                        child.has_data = true;
                    } else if (child_entry.is_array()) {
                        try { child.child_index = child_entry[0].get<int>(); } catch (...) { child.child_index = -1; }
                        if (child_entry.size() > 1 && child_entry[1].is_number()) {
                            child.dx = static_cast<float>(child_entry[1].get<double>());
                        }
                        if (child_entry.size() > 2 && child_entry[2].is_number()) {
                            child.dy = static_cast<float>(child_entry[2].get<double>());
                        }
                        if (child_entry.size() > 3 && child_entry[3].is_number()) {
                            child.dz = static_cast<float>(child_entry[3].get<double>());
                            if (child_entry.size() > 4 && child_entry[4].is_number()) {
                                child.degree = static_cast<float>(child_entry[4].get<double>());
                            }
                            if (child_entry.size() > 5) {
                                if (child_entry[5].is_boolean()) {
                                    child.visible = child_entry[5].get<bool>();
                                } else if (child_entry[5].is_number_integer()) {
                                    child.visible = child_entry[5].get<int>() != 0;
                                }
                            }
                        } else {
                            if (child_entry.size() > 3 && child_entry[3].is_number()) {
                                child.degree = static_cast<float>(child_entry[3].get<double>());
                            }
                            if (child_entry.size() > 4) {
                                if (child_entry[4].is_boolean()) {
                                    child.visible = child_entry[4].get<bool>();
                                } else if (child_entry[4].is_number_integer()) {
                                    child.visible = child_entry[4].get<int>() != 0;
                                }
                            }
                            child.dz = child.dy;
                            child.dy = 0.0f;
                        }
                        child.has_data = true;
                    }
                    f.children.push_back(child);
                }
            }
        }

        f.hit.boxes.clear();
        if (hit_geom.is_array() && frame_index < hit_geom.size()) {
            const auto& hit_entry = hit_geom[static_cast<nlohmann::json::size_type>(frame_index)];
            if (hit_entry.is_object()) {
                if (hit_entry.contains("boxes") && hit_entry["boxes"].is_array()) {
                    for (const auto& node : hit_entry["boxes"]) {
                        append_hit_box(f, node);
                    }
                } else if (hit_entry.contains("center_x") || hit_entry.contains("half_width") || hit_entry.contains("rotation")) {
                    append_hit_box(f, hit_entry);
                } else {
                    for (const char* type : kLegacyDamageTypeNames) {
                        auto it = hit_entry.find(type);
                        if (it != hit_entry.end()) {
                            append_hit_box(f, *it);
                        }
                    }
                }
            } else if (hit_entry.is_array()) {
                if (!hit_entry.empty() && hit_entry.front().is_object()) {
                    for (const auto& node : hit_entry) {
                        append_hit_box(f, node);
                    }
                } else if (!hit_entry.is_null()) {
                    append_hit_box(f, hit_entry);
                }
            }
        }

        f.attack.vectors.clear();
        if (attack_geom.is_array() && frame_index < attack_geom.size()) {
            const auto& attack_entry = attack_geom[static_cast<nlohmann::json::size_type>(frame_index)];
            if (attack_entry.is_object()) {
                if (attack_entry.contains("vectors") && attack_entry["vectors"].is_array()) {
                    for (const auto& vec_node : attack_entry["vectors"]) {
                        append_attack_vector(f, vec_node);
                    }
                } else {
                    for (const char* type : kLegacyDamageTypeNames) {
                        auto it = attack_entry.find(type);
                        if (it == attack_entry.end() || !it->is_array()) continue;
                        for (const auto& vec_node : *it) {
                            append_attack_vector(f, vec_node);
                        }
                    }
                }
            } else if (attack_entry.is_array()) {
                for (const auto& vec_node : attack_entry) {
                    append_attack_vector(f, vec_node);
                }
            }
        }

        frames.push_back(clamp_frame(f));
        ++frame_index;
    }
    if (frames.empty()) frames.push_back(MovementFrame{});
    return frames;
}
