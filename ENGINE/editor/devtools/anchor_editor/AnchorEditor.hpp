#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "devtools/frame_editors/FrameEditorBase.hpp"
#include "devtools/frame_editors/shared/AnchorFrameState.hpp"
#include "devtools/frame_editors/shared/FrameEditorContext.hpp"
#include "devtools/frame_editors/shared/FrameNavigator.hpp"
#include "devtools/frame_editors/shared/ManifestTransaction.hpp"
#include "devtools/frame_editors/shared/ToolPanel.hpp"
#include "devtools/widgets.hpp"

namespace devmode::anchor_editor {

class AnchorListWidget;
struct AnchorListWidgetDeleter {
    void operator()(AnchorListWidget* ptr) const noexcept;
};

class AnchorEditor : public frame_editors::FrameEditorBase {
public:
    void begin(const frame_editors::FrameEditorContext& context) override;
    void end() override;
    bool handle_event(const SDL_Event& e) override;
    void update(const Input& input, float dt) override;
    void render_world(SDL_Renderer* renderer) const override;
    void render_overlays(SDL_Renderer* renderer) const override;
    void persist_pending_changes() override;
    bool wants_close() const override { return wants_close_; }

private:
    void layout_ui(SDL_Renderer* renderer = nullptr) const;
    void rebuild_tool_panel_layout();
    void refresh_form();
    bool apply_form_to_anchor();
    void select_frame(int index);
    void select_anchor(int index);
    void add_anchor();
    void apply_to_all_frames();
    void apply_to_next_frame();
    void apply_to_animation();
    bool apply_to_selected_animations();
    bool apply_to_all_animations();
    void persist_changes();
    void invalidate_preview() const;
    void refresh_selection_state();
    void request_close();
    void prime_textures();
    bool ui_contains_point(const SDL_Point& p) const;
    std::pair<int, int> frame_dimensions_for_index(std::size_t frame_index) const;
    std::pair<int, int> current_frame_dimensions() const;
    void hydrate_anchor_pixels_from_target();
    DisplacedAssetAnchorPoint to_runtime_anchor(const frame_editors::FrameAnchorPoint& anchor) const;
    void sync_runtime_animation_anchors(const std::string& animation_id,
                                        const std::vector<frame_editors::AnchorFrame>& frames);

    struct FrameTextureInfo {
        SDL_Texture* texture = nullptr;
        SDL_Rect src_rect{0, 0, 0, 0};
        bool has_src_rect = false;
    };
    FrameTextureInfo resolve_frame_texture(SDL_Renderer* renderer, int frame_index) const;

    bool point_in_viewport(const SDL_Point& p) const;
    SDL_FPoint texture_to_screen(int tx, int ty) const;
    bool screen_to_texture(const SDL_Point& p, int& tx, int& ty) const;
    void center_view();
    void update_drag_pick(const SDL_Point& p);
    SDL_Texture* current_frame_texture() const;

    frame_editors::FrameEditorContext context_{};
    frame_editors::ManifestTransaction manifest_txn_{};
    frame_editors::SelectionState* selection_state_ = nullptr;
    std::vector<frame_editors::AnchorFrame> frames_;
    int selected_frame_ = 0;
    int selected_anchor_ = -1;
    bool wants_close_ = false;
    bool dirty_ = false;
    bool texture_primed_ = false;
    bool tried_runtime_rebuild_ = false;
    bool preview_refreshed_ = false;

    bool is_dragging_anchor_ = false;
    bool is_panning_ = false;
    SDL_Point pan_last_{0, 0};

    mutable int screen_w_ = 1920;
    mutable int screen_h_ = 1080;
    mutable SDL_Rect nav_rect_{0, 0, 0, 0};
    mutable SDL_Rect viewport_rect_{0, 0, 0, 0};
    float zoom_ = 4.0f;
    SDL_FPoint pan_{0.0f, 0.0f};

    std::unique_ptr<frame_editors::FrameNavigator> frame_navigator_;
    std::unique_ptr<DMButton> btn_back_;
    std::unique_ptr<DMButton> btn_add_;
    std::unique_ptr<DMButton> btn_delete_;
    std::unique_ptr<DMButton> btn_save_;

    std::unique_ptr<DMTextBox> tb_name_;
    std::unique_ptr<DMTextBox> tb_tex_x_;
    std::unique_ptr<DMTextBox> tb_tex_y_;
    std::unique_ptr<DMCheckbox> cb_in_front_;

    std::unique_ptr<frame_editors::FrameToolPanel> tool_panel_;
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

}  // namespace devmode::anchor_editor
