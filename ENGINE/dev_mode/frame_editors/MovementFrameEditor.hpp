#pragma once

#include <memory>
#include <string>
#include <vector>

#include "FrameEditorBase.hpp"
#include "shared/AxisAdjuster.hpp"
#include "shared/FrameEditState.hpp"
#include "shared/FrameEditorContext.hpp"
#include "shared/ManifestTransaction.hpp"
#include "shared/SelectionState.hpp"

namespace devmode::frame_editors {

class DMButton;
class DMCheckbox;
class DMTextBox;

class MovementFrameEditor : public FrameEditorBase {
public:
    MovementFrameEditor() = default;

    void begin(const FrameEditorContext& context) override;
    void end() override;
    bool handle_event(const SDL_Event& e) override;
    void update(const Input& input, float dt) override;
    void render_world(SDL_Renderer* renderer) const override;
    void render_overlays(SDL_Renderer* renderer) const override;
    bool wants_close() const override { return wants_close_; }

private:
    void layout_ui(SDL_Renderer* renderer) const;
    void sync_text_fields();
    void apply_text_field_changes();
    void select_frame(int index);
    void rebuild_rel_positions();
    void apply_frame_move_from_base(int index, SDL_FPoint desired_rel, const std::vector<SDL_FPoint>& base_rel);
    void redistribute_frames_after_adjustment(int adjusted_index);
    void apply_linear_smoothing(int adjusted_index, std::vector<SDL_FPoint>& redistributed, int last_index) const;
    void apply_curved_smoothing(int adjusted_index,
                                const std::vector<SDL_FPoint>& original,
                                std::vector<SDL_FPoint>& redistributed,
                                int last_index) const;
    void persist_changes();
    void apply_to_all_frames();
    void refresh_selection_state();
    SDL_Point asset_anchor_world() const;
    SDL_FPoint screen_to_world_relative(const SDL_Point& screen) const;
    bool ui_contains_point(const SDL_Point& pt) const;

    FrameEditorContext context_{};
    SelectionState* selection_state_ = nullptr;
    AxisAdjuster* axis_adjuster_ = nullptr;
    ManifestTransaction manifest_txn_;
    std::vector<MovementFrame> frames_;
    std::vector<SDL_FPoint> rel_positions_;
    int selected_index_ = 0;
    bool wants_close_ = false;

    mutable SDL_Rect ui_rect_{0, 0, 0, 0};
    mutable SDL_Rect controls_rect_{0, 0, 0, 0};
    mutable SDL_Rect fields_rect_{0, 0, 0, 0};

    std::unique_ptr<DMCheckbox> cb_smooth_;
    std::unique_ptr<DMCheckbox> cb_curve_;
    std::unique_ptr<DMButton> btn_apply_all_;
    std::unique_ptr<DMButton> btn_prev_frame_;
    std::unique_ptr<DMButton> btn_next_frame_;
    std::unique_ptr<DMTextBox> tb_dx_;
    std::unique_ptr<DMTextBox> tb_dy_;
    mutable std::string last_dx_text_{};
    mutable std::string last_dy_text_{};
    bool dragging_point_ = false;
    SDL_Point drag_start_screen_{0, 0};
    SDL_FPoint drag_start_rel_{0.0f, 0.0f};
    bool smooth_enabled_ = false;
    bool curve_enabled_ = false;
};

}  // namespace devmode::frame_editors
