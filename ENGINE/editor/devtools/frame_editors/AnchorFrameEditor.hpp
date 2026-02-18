#pragma once

#include <memory>
#include <string>
#include <vector>

#include "FrameEditorBase.hpp"
#include "shared/AnchorFrameState.hpp"
#include "shared/FrameEditorContext.hpp"
#include "shared/FrameNavigator.hpp"
#include "shared/Point3DEditor.hpp"
#include "shared/ToolPanel.hpp"
#include "shared/ManifestTransaction.hpp"
#include "devtools/asset_editor/animation_editor_window/AnimationDocument.hpp"
#include "devtools/widgets.hpp"

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
    void apply_to_all_frames();
    void apply_to_next_frame();
    bool apply_to_all_animations();
    void apply_to_animation();
    void persist_changes();
    void apply_live_changes(bool force = false);
    void invalidate_preview() const;
    void refresh_selection_state();
    void update_selected_anchor_from_world(const SDL_FPoint& world_pos, float world_z);
    float parent_height_px() const;
    bool resolve_anchor_screen(int anchor_index, SDL_FPoint& out_screen, float& out_world_z, SDL_FPoint* out_world = nullptr) const;
    bool ui_contains_point(const SDL_Point& p) const;
    void add_anchor();
    void ensure_anchor_exists_everywhere(const std::string& name);
    void remove_anchor_everywhere(const std::string& name);
    void rename_anchor_everywhere(const std::string& old_name, const std::string& new_name);
    void rebuild_tool_panel_layout();

    FrameEditorContext context_{};
    ManifestTransaction manifest_txn_{};
    SelectionState* selection_state_ = nullptr;
    std::vector<AnchorFrame> frames_;
    int selected_frame_ = 0;
    int selected_anchor_ = -1;
    bool wants_close_ = false;
    bool dirty_ = false;

    std::unique_ptr<FrameNavigator> frame_navigator_;
    std::unique_ptr<DMButton> btn_back_;
    std::unique_ptr<DMButton> btn_add_;
    std::unique_ptr<DMButton> btn_delete_;
    std::unique_ptr<Point3DEditor> point_3d_editor_;

    std::unique_ptr<DMTextBox> tb_name_;
    std::unique_ptr<DMTextBox> tb_px_;
    std::unique_ptr<DMTextBox> tb_py_;
    std::unique_ptr<DMTextBox> tb_pz_;
    std::unique_ptr<DMTextBox> tb_rot_;

    mutable SDL_Rect nav_rect_{0, 0, 0, 0};
    mutable int screen_w_ = 1920;
    mutable int screen_h_ = 1080;

    std::unique_ptr<FrameToolPanel> tool_panel_;
    std::unique_ptr<ButtonWidget> back_widget_;
    std::unique_ptr<ButtonWidget> add_widget_;
    std::unique_ptr<ButtonWidget> delete_widget_;
    std::unique_ptr<TextBoxWidget> name_widget_;
    std::unique_ptr<TextBoxWidget> px_widget_;
    std::unique_ptr<TextBoxWidget> py_widget_;
    std::unique_ptr<TextBoxWidget> pz_widget_;
    std::unique_ptr<TextBoxWidget> rot_widget_;
    std::unique_ptr<AnchorListWidget, AnchorListWidgetDeleter> anchor_list_widget_;
};

}  // namespace devmode::frame_editors
