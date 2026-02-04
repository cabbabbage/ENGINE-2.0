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
    void apply_to_all_frames();
    void refresh_selection_state();
    SDL_Point asset_anchor_world() const;
    SDL_FPoint screen_to_world_relative(const SDL_Point& screen) const;
    float base_world_z() const;
    bool project_relative_point(std::size_t idx, SDL_FPoint& out) const;
    bool ui_contains_point(const SDL_Point& pt) const;

    FrameEditorContext context_{};
    SelectionState* selection_state_ = nullptr;
    std::unique_ptr<Point3DEditor> point_3d_editor_;
    ManifestTransaction manifest_txn_;
    std::vector<MovementFrame> frames_;
    std::vector<SDL_FPoint> rel_positions_;
    std::vector<float> rel_positions_z_;
    int selected_index_ = 0;
    bool wants_close_ = false;

    mutable SDL_Rect ui_rect_{0, 0, 0, 0};

    std::unique_ptr<DMCheckbox> cb_smooth_;
    std::unique_ptr<DMCheckbox> cb_curve_;
    std::unique_ptr<DMButton> btn_back_;
    std::unique_ptr<DMButton> btn_apply_all_;
    std::unique_ptr<FrameNavigator> frame_navigator_;
    bool smooth_enabled_ = false;
    bool curve_enabled_ = false;
};

}  // namespace devmode::frame_editors
