#pragma once

#include <array>
#include <memory>
#include <string>
#include <vector>

#include "FrameEditorBase.hpp"
#include "shared/AxisAdjuster.hpp"
#include "shared/FrameEditState.hpp"
#include "shared/FrameEditorContext.hpp"
#include "shared/ManifestTransaction.hpp"
#include "shared/SelectionState.hpp"

class DMButton;
class DMDropdown;
class DMTextBox;

namespace devmode::frame_editors {

class HitGeoFrameEditor : public FrameEditorBase {
public:
    void begin(const FrameEditorContext& context) override;
    void end() override;
    bool handle_event(const SDL_Event& e) override;
    void update(const Input& input, float dt) override;
    void render_world(SDL_Renderer* renderer) const override;
    void render_overlays(SDL_Renderer* renderer) const override;
    bool wants_close() const override { return wants_close_; }

private:
    enum class HitHandle { None, Move, Left, Right, Top, Bottom, Rotate };

    void layout_ui(SDL_Renderer* renderer) const;
    void select_frame(int index);
    void refresh_hitbox_form() const;
    void apply_text_fields();
    void persist_changes();
    void apply_scroll_adjustment(int steps);
    void apply_hit_to_all_frames();
    void copy_hit_box_to_next_frame();
    std::string current_hitbox_type() const;
    animation_update::FrameHitGeometry::HitBox* current_hit_box();
    const animation_update::FrameHitGeometry::HitBox* current_hit_box() const;
    animation_update::FrameHitGeometry::HitBox* ensure_hit_box_for_type(const std::string& type);
    void delete_hit_box_for_type(const std::string& type);
    bool begin_hitbox_drag(SDL_Point mouse);
    void update_hitbox_drag(SDL_Point mouse);
    void end_hitbox_drag(bool commit);
    bool build_hitbox_visual(const animation_update::FrameHitGeometry::HitBox& box,
                             std::array<SDL_FPoint, 4>& corners,
                             std::array<SDL_FPoint, 4>& edge_midpoints,
                             SDL_FPoint& rotate_handle) const;
    void render_hit_geometry(SDL_Renderer* renderer) const;
    SDL_Point asset_anchor_world() const;
    float asset_local_scale() const;
    bool screen_to_local(SDL_Point screen, SDL_FPoint& out_local) const;
    void refresh_selection_state();
    bool ui_contains_point(const SDL_Point& p) const;

    FrameEditorContext context_{};
    SelectionState* selection_state_ = nullptr;
    AxisAdjuster* axis_adjuster_ = nullptr;
    ManifestTransaction manifest_txn_;
    std::vector<MovementFrame> frames_;
    int selected_index_ = 0;
    int selected_hitbox_type_index_ = 1;

    std::unique_ptr<DMDropdown> dd_hitbox_type_;
    std::unique_ptr<DMButton> btn_back_;
    std::unique_ptr<DMButton> btn_prev_frame_;
    std::unique_ptr<DMButton> btn_next_frame_;
    std::unique_ptr<DMButton> btn_add_remove_;
    std::unique_ptr<DMButton> btn_copy_next_;
    std::unique_ptr<DMButton> btn_apply_all_;
    std::unique_ptr<DMTextBox> tb_center_x_;
    std::unique_ptr<DMTextBox> tb_center_y_;
    std::unique_ptr<DMTextBox> tb_center_z_;
    std::unique_ptr<DMTextBox> tb_width_;
    std::unique_ptr<DMTextBox> tb_height_;
    std::unique_ptr<DMTextBox> tb_rotation_;
    mutable std::string last_center_x_;
    mutable std::string last_center_y_;
    mutable std::string last_center_z_;
    mutable std::string last_width_;
    mutable std::string last_height_;
    mutable std::string last_rotation_;

    mutable SDL_Rect ui_rect_{0, 0, 0, 0};

    HitHandle active_handle_ = HitHandle::None;
    bool dragging_hitbox_ = false;
    bool drag_moved_ = false;
    bool wants_close_ = false;
    SDL_Point drag_start_mouse_{0, 0};
    SDL_FPoint drag_grab_offset_{0.0f, 0.0f};
    animation_update::FrameHitGeometry::HitBox drag_start_box_{};
    float drag_left_ = 0.0f;
    float drag_right_ = 0.0f;
    float drag_top_ = 0.0f;
    float drag_bottom_ = 0.0f;
};

}  // namespace devmode::frame_editors
