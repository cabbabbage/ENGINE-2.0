#pragma once

#include <memory>
#include <optional>
#include <vector>

#include "dev_mode/frame_editors/FrameEditorBase.hpp"
#include "animation_update/child_attachment_controller.hpp"
#include "dev_mode/asset_sections/animation_editor_window/AnimationDocument.hpp"
#include "asset/animation_child_data.hpp"
#include "shared/AxisAdjuster.hpp"
#include "shared/ChildTimelineUtils.hpp"
#include "shared/FrameEditorContext.hpp"
#include "shared/ManifestTransaction.hpp"
#include "shared/SelectionState.hpp"

class DMButton;

namespace devmode::frame_editors {

class AsyncChildrenFrameEditor final : public FrameEditorBase {
public:
    void begin(const FrameEditorContext& context) override;
    void end() override;
    bool handle_event(const SDL_Event& e) override;
    void update(const Input& input, float dt) override;
    void render_world(SDL_Renderer* renderer) const override;
    void render_overlays(SDL_Renderer* renderer) const override;
    bool wants_close() const override { return wants_close_; }

private:
    void populate_child_data();
    void ensure_manifest_transaction();
    void apply_preview();
    float attachment_scale() const;
    SDL_Point asset_anchor_world() const;
    struct ChildWorldPose {
        SDL_FPoint pos{0.0f, 0.0f};
        float z = 0.0f;
    };
    ChildWorldPose child_world_pose(int child_index) const;
    std::optional<int> hit_test_child_marker(const SDL_Point& mouse) const;
    child_timelines::ChildFrameSample sample_for_child(int child_index, bool for_preview) const;
    child_timelines::ChildFrameSample default_sample(int child_index) const;
    int mapped_child_frame_index(int child_index) const;
    void ensure_async_frame_capacity(int child_index, int frame_index);
    float start_time_for_child(int child_index) const;
    int start_frame_for_child(int child_index) const;
    void adjust_start_frame(int child_index, int delta_frames);
    void refresh_selection_state();
    void apply_scroll_adjustment(int steps);

    FrameEditorContext context_{};
    SelectionState* selection_state_ = nullptr;
    AxisAdjuster* axis_adjuster_ = nullptr;
    ManifestTransaction manifest_txn_;

    std::vector<std::string> child_assets_;
    std::vector<AnimationChildMode> child_modes_;
    std::vector<std::vector<child_timelines::ChildFrameSample>> static_frames_by_child_;
    std::vector<std::vector<child_timelines::ChildFrameSample>> async_frames_by_child_;
    std::vector<float> async_start_times_;
    std::vector<int> async_start_frames_;
    std::vector<bool> async_has_start_;

    int parent_frame_count_ = 0;
    int selected_parent_frame_index_ = 0;
    int selected_child_index_ = 0;
    int selected_child_frame_index_ = 0;
    bool dragging_child_ = false;
    SDL_Point drag_start_mouse_{0, 0};
    child_timelines::ChildFrameSample drag_start_sample_{};
    bool data_dirty_ = false;
    bool wants_close_ = false;
    std::unique_ptr<DMButton> btn_back_;
    mutable SDL_Rect back_rect_{0, 0, 0, 0};
};

}  // namespace devmode::frame_editors
