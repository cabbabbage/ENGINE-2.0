#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <SDL.h>

#include "FrameEditorBase.hpp"
#include "animation_update/child_attachment_controller.hpp"
#include "dev_mode/asset_sections/animation_editor_window/AnimationDocument.hpp"
#include "animation_update/animation_update.hpp"
#include "asset/animation_child_data.hpp"
#include "shared/AxisAdjuster.hpp"
#include "shared/ChildTimelineUtils.hpp"
#include "shared/FrameEditorContext.hpp"
#include "shared/FrameNavigator.hpp"
#include "shared/ManifestTransaction.hpp"
#include "shared/SelectionState.hpp"
#include "dev_mode/widgets.hpp"

class DMDropdown;
class DMCheckbox;
class DMTextBox;

class DMButton;

namespace devmode::frame_editors {

class SyncChildrenFrameEditor : public FrameEditorBase {
public:
    SyncChildrenFrameEditor() = default;

    void begin(const FrameEditorContext& context) override;
    void end() override;
    bool handle_event(const SDL_Event& e) override;
    void update(const Input& input, float dt) override;
    void render_world(SDL_Renderer* renderer) const override;
    void render_overlays(SDL_Renderer* renderer) const override;
    bool wants_close() const override { return wants_close_; }

private:
    void initialize();
    void populate_child_data();
    void apply_current_frame_to_children();
    float attachment_scale() const;
    SDL_Point asset_anchor_world() const;
    struct ChildWorldPose {
        SDL_FPoint pos{0.0f, 0.0f};
        float z = 0.0f;
    };
    ChildWorldPose child_world_pose(int child_index) const;
    std::optional<int> hit_test_child_marker(const SDL_Point& mouse) const;
    void ensure_manifest_transaction();
    void apply_scroll_adjustment(int steps);
    void apply_text_box_changes();
    void update_text_boxes_from_current_frame();

    FrameEditorContext context_{};
    SelectionState* selection_state_ = nullptr;
    AxisAdjuster* axis_adjuster_ = nullptr;
    ManifestTransaction manifest_txn_;

    std::vector<std::string> child_assets_;
    std::vector<AnimationChildMode> child_modes_;
    std::vector<std::vector<child_timelines::ChildFrameSample>> static_frames_by_child_;
    std::vector<std::vector<child_timelines::ChildFrameSample>> async_timelines_by_child_;
    int frame_count_ = 0;
    int selected_frame_index_ = 0;
    int selected_child_index_ = 0;
    bool dragging_child_ = false;
    SDL_Point drag_start_mouse_{0, 0};
    child_timelines::ChildFrameSample drag_start_sample_{};
    bool data_dirty_ = false;
    bool wants_close_ = false;
    std::unique_ptr<DMButton> btn_back_;
    std::unique_ptr<FrameNavigator> frame_navigator_;
    std::unique_ptr<DMDropdown> dd_child_selector_;
    std::unique_ptr<CallbackCheckboxWidget> cb_child_visible_;
    std::unique_ptr<DMTextBox> tb_dx_;
    std::unique_ptr<DMTextBox> tb_dy_;
    std::unique_ptr<DMTextBox> tb_dz_;
    std::unique_ptr<DMTextBox> tb_rotation_;
    mutable SDL_Rect back_rect_{0, 0, 0, 0};
    mutable SDL_Rect ui_rect_{0, 0, 0, 0};
};

}  // namespace devmode::frame_editors
