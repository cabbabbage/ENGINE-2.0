#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "sdl3_render_compat.hpp"

#include "FrameEditorBase.hpp"
#include "animation/child_attachment_controller.hpp"
#include "devtools/asset_editor/animation_editor_window/AnimationDocument.hpp"
#include "animation/animation_update.hpp"
#include "assets/animation_child_data.hpp"
#include "shared/Point3DEditor.hpp"
#include "shared/ChildTimelineUtils.hpp"
#include "shared/FrameEditorContext.hpp"
#include "shared/FrameNavigator.hpp"
#include "shared/ManifestTransaction.hpp"
#include "shared/SelectionState.hpp"
#include "devtools/widgets.hpp"

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
    void persist_pending_changes() override;
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
    std::vector<int> static_child_point_indices() const;
    int child_index_from_point_index(int point_index) const;
    int point_index_for_child(int child_index) const;
    void ensure_manifest_transaction();
    void apply_live_changes();
    void force_save_to_disk();
    void invalidate_preview() const;
    void refresh_selection_state();
    void sync_visibility_checkbox();
    void reset_current_frame();

    FrameEditorContext context_{};
    SelectionState* selection_state_ = nullptr;
    std::unique_ptr<Point3DEditor> point_3d_editor_;
    ManifestTransaction manifest_txn_;

    std::vector<std::string> child_assets_;
    std::vector<AnimationChildMode> child_modes_;
    std::vector<std::vector<child_timelines::ChildFrameSample>> static_frames_by_child_;
    std::vector<std::vector<child_timelines::ChildFrameSample>> async_timelines_by_child_;
    int frame_count_ = 0;
    int selected_frame_index_ = 0;
    int selected_child_index_ = 0;
    bool data_dirty_ = false;
    bool wants_close_ = false;
    std::unique_ptr<DMButton> btn_back_;
    std::unique_ptr<DMButton> btn_reset_frame_;
    std::unique_ptr<FrameNavigator> frame_navigator_;
    std::unique_ptr<DMDropdown> dd_child_selector_;
    std::unique_ptr<CallbackCheckboxWidget> cb_child_visible_;
    mutable SDL_Rect back_rect_{0, 0, 0, 0};
    mutable SDL_Rect ui_rect_{0, 0, 0, 0};

    // Cache the camera perspective scale from session start to prevent camera movement
    // from affecting child position calculations
    float cached_perspective_scale_ = 1.0f;
    // Cache the full attachment scale at session start so edits stay stable if the camera moves
    float cached_attachment_scale_ = 1.0f;
};

}  // namespace devmode::frame_editors
