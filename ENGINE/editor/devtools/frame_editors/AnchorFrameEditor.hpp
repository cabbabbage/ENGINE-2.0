#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "FrameEditorBase.hpp"
#include "shared/AnchorFrameState.hpp"
#include "shared/FrameEditorContext.hpp"
#include "shared/FrameNavigator.hpp"
#include "shared/ToolPanel.hpp"
#include "shared/ManifestTransaction.hpp"
#include "devtools/asset_editor/animation_editor_window/AnimationDocument.hpp"
#include "devtools/widgets.hpp"

class AnimationFrame;

namespace devmode::frame_editors {

class AnchorListWidget;
struct AnchorListWidgetDeleter {
    void operator()(AnchorListWidget* ptr) const noexcept;
};

class AnchorFrameEditor : public FrameEditorBase {
public:
    AnchorFrameEditor() = default;
    ~AnchorFrameEditor();

    void begin(const FrameEditorContext& context) override;
    void end() override;
    bool handle_event(const SDL_Event& e) override;
    void update(const Input& input, float dt) override;
    void render_world(SDL_Renderer* renderer) const override;
    void render_overlays(SDL_Renderer* renderer) const override;
    void persist_pending_changes() override;
    bool wants_close() const override { return wants_close_; }

private:
    void layout_ui(SDL_Renderer* renderer = nullptr) const;
    void select_frame(int index);
    void select_anchor(int index);
    void refresh_form();
    bool apply_form_to_anchor();
    bool apply_scope_to_next_frame();
    bool apply_scope_to_animation(const std::string& animation_id);
    bool apply_scope_to_selected_animations();
    bool apply_scope_to_all_animations();
    bool apply_scope_to_all_frames_in_animation(const std::string& animation_id);
    bool persist_payload_for_animation(const std::string& animation_id, const std::vector<AnchorFrame>& frames, bool save_to_disk);
    void checkpoint_undo(const std::string& label) const;
    void persist_changes();
    void apply_live_changes(bool force = false);
    void invalidate_preview() const;
    void refresh_selection_state();
    bool resolve_anchor_screen(int anchor_index, SDL_FPoint& out_screen, float& out_world_z, SDL_FPoint* out_world = nullptr) const;
    bool ui_contains_point(const SDL_Point& p) const;
    void add_anchor();
    void rebuild_tool_panel_layout();
    void hydrate_anchor_pixels_from_target();
    std::pair<int, int> current_frame_dimensions() const;
    std::pair<int, int> frame_dimensions_for_index(std::size_t frame_index) const;
    DisplacedAssetAnchorPoint to_runtime_anchor(const FrameAnchorPoint& anchor) const;
    void lock_target_to_selected_frame();
    void restore_target_frame_lock();
    void sync_runtime_animation_anchors(const std::string& animation_id, const std::vector<AnchorFrame>& frames);

    FrameEditorContext context_{};
    ManifestTransaction manifest_txn_{};
    SelectionState* selection_state_ = nullptr;
    std::vector<AnchorFrame> frames_;
    int selected_frame_ = 0;
    int selected_anchor_ = -1;
    bool wants_close_ = false;
    bool dirty_ = false;
    bool target_frame_lock_active_ = false;
    std::string saved_target_animation_;
    AnimationFrame* saved_target_frame_ = nullptr;
    bool saved_target_static_frame_ = false;

    std::unique_ptr<FrameNavigator> frame_navigator_;
    std::unique_ptr<DMButton> btn_back_;
    std::unique_ptr<DMButton> btn_add_;
    std::unique_ptr<DMButton> btn_delete_;
    std::unique_ptr<DMButton> btn_save_;

    std::unique_ptr<DMTextBox> tb_name_;
    std::unique_ptr<DMTextBox> tb_tex_x_;
    std::unique_ptr<DMTextBox> tb_tex_y_;
    std::unique_ptr<DMCheckbox> cb_in_front_;

    mutable SDL_Rect nav_rect_{0, 0, 0, 0};
    mutable int screen_w_ = 1920;
    mutable int screen_h_ = 1080;

    std::unique_ptr<FrameToolPanel> tool_panel_;
    std::unique_ptr<ButtonWidget> back_widget_;
    std::unique_ptr<ButtonWidget> add_widget_;
    std::unique_ptr<ButtonWidget> delete_widget_;
    std::unique_ptr<ButtonWidget> save_widget_;
    std::unique_ptr<TextBoxWidget> name_widget_;
    std::unique_ptr<TextBoxWidget> tex_x_widget_;
    std::unique_ptr<TextBoxWidget> tex_y_widget_;
    std::unique_ptr<CheckboxWidget> in_front_widget_;
    std::unique_ptr<AnchorListWidget, AnchorListWidgetDeleter> anchor_list_widget_;
};

}  // namespace devmode::frame_editors
