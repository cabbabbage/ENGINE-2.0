#pragma once

#include <array>
#include <memory>
#include <string>
#include <vector>

#include "FrameEditorBase.hpp"
#include "shared/Point3DEditor.hpp"
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
    void persist_pending_changes() override;
    bool wants_close() const override { return wants_close_; }

private:
    void layout_ui(SDL_Renderer* renderer) const;
    void select_frame(int index);
    void refresh_hitbox_form() const;
    void persist_changes();
    void apply_live_changes();
    void invalidate_preview() const;
    void apply_hit_to_all_frames();
    void copy_hit_box_to_next_frame();
    std::string current_hitbox_type() const;
    float base_world_z() const;
    animation_update::FrameHitGeometry::HitBox* current_hit_box();
    const animation_update::FrameHitGeometry::HitBox* current_hit_box() const;
    animation_update::FrameHitGeometry::HitBox* ensure_hit_box_for_type(const std::string& type);
    void delete_hit_box_for_type(const std::string& type);
    bool build_hitbox_visual(const animation_update::FrameHitGeometry::HitBox& box,
                             std::array<SDL_FPoint, 4>& corners,
                             std::array<SDL_FPoint, 4>& edge_midpoints,
                             SDL_FPoint& rotate_handle) const;
    void render_hit_geometry(SDL_Renderer* renderer) const;
    SDL_Point asset_anchor_world() const;
    float asset_local_scale() const;
    bool screen_to_local(SDL_Point screen, SDL_FPoint& out_local) const;
    SDL_FPoint screen_to_world_point(const SDL_Point& screen) const;
    void refresh_selection_state();
    bool ui_contains_point(const SDL_Point& p) const;

    FrameEditorContext context_{};
    SelectionState* selection_state_ = nullptr;
    std::unique_ptr<Point3DEditor> point_3d_editor_;
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

    mutable SDL_Rect ui_rect_{0, 0, 0, 0};

    bool wants_close_ = false;
};

}  // namespace devmode::frame_editors
