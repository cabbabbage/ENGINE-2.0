#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "FrameEditorBase.hpp"
#include "shared/Point3DEditor.hpp"
#include "shared/FrameEditState.hpp"
#include <memory>
#include "shared/FrameEditorContext.hpp"
#include "shared/FrameNavigator.hpp"
#include "shared/ToolPanel.hpp"
#include "shared/ManifestTransaction.hpp"
#include "shared/SelectionState.hpp"
#include "devtools/widgets.hpp"

namespace devmode::frame_editors {

class MovementFrameEditor : public FrameEditorBase {
public:
    MovementFrameEditor() = default;

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
    void rebuild_rel_positions();
    void redistribute_frames_after_adjustment(int adjusted_index);
    void apply_linear_smoothing(int adjusted_index, std::vector<SDL_FPoint>& redistributed, int last_index) const;
    void apply_curved_smoothing(int adjusted_index,
                                const std::vector<SDL_FPoint>& original,
                                std::vector<SDL_FPoint>& redistributed,
                                int last_index) const;
    void persist_changes();
    void apply_live_changes();
    void invalidate_preview() const;
    void sync_current_path_from_frames();
    void select_path(int index);
    void add_movement_path();
    void delete_selected_movement_path();
    void update_path_button_labels();
    void apply_movement_to_next_frame();
    void apply_movement_to_animation();
    bool apply_movement_to_all_animations();
    void copy_movement_fields(MovementFrame& dest, const MovementFrame& src) const;
    void refresh_selection_state();
    void apply_selected_frame_to_target();
    SDL_Point asset_anchor_world() const;
    SDL_FPoint screen_to_world_relative(const SDL_Point& screen) const;
    float base_world_z() const;
    bool project_relative_point(std::size_t idx, SDL_FPoint& out) const;
    bool ui_contains_point(const SDL_Point& pt) const;

    FrameEditorContext context_{};
    SelectionState* selection_state_ = nullptr;
    std::unique_ptr<Point3DEditor> point_3d_editor_;
    ManifestTransaction manifest_txn_;
    std::vector<std::vector<MovementFrame>> movement_paths_;
    std::vector<MovementFrame> frames_;
    std::vector<SDL_FPoint> rel_positions_;
    std::vector<float> rel_positions_z_;
    int selected_path_index_ = 0;
    int selected_index_ = 0;
    bool dirty_ = false;
    bool wants_close_ = false;

    mutable SDL_Rect nav_rect_{0, 0, 0, 0};
    mutable int screen_w_ = 1920;
    mutable int screen_h_ = 1080;

    std::unique_ptr<DMCheckbox> cb_smooth_;
    std::unique_ptr<DMCheckbox> cb_curve_;
    std::unique_ptr<DMButton> btn_prev_path_;
    std::unique_ptr<DMButton> btn_path_label_;
    std::unique_ptr<DMButton> btn_next_path_;
    std::unique_ptr<DMButton> btn_add_path_;
    std::unique_ptr<DMButton> btn_delete_path_;
    std::unique_ptr<DMButton> btn_back_;
    std::unique_ptr<FrameNavigator> frame_navigator_;
    bool smooth_enabled_ = false;
    bool curve_enabled_ = false;
    bool had_previous_static_frame_ = false;
    bool previous_static_frame_ = false;

    std::unique_ptr<FrameToolPanel> tool_panel_;
    std::unique_ptr<ButtonWidget> back_widget_;
    std::unique_ptr<ButtonWidget> prev_path_widget_;
    std::unique_ptr<ButtonWidget> path_label_widget_;
    std::unique_ptr<ButtonWidget> next_path_widget_;
    std::unique_ptr<ButtonWidget> add_path_widget_;
    std::unique_ptr<ButtonWidget> delete_path_widget_;
    std::unique_ptr<CheckboxWidget> smooth_widget_;
    std::unique_ptr<CheckboxWidget> curve_widget_;
};

}  // namespace devmode::frame_editors
