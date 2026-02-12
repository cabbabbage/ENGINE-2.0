#pragma once

#include <memory>
#include <string>
#include <vector>

#include "FrameEditorBase.hpp"
#include "shared/AnchorFrameState.hpp"
#include "shared/FrameEditorContext.hpp"
#include "shared/FrameNavigator.hpp"
#include "shared/Point3DEditor.hpp"
#include "shared/ManifestTransaction.hpp"
#include "devtools/asset_editor/animation_editor_window/AnimationDocument.hpp"
#include "devtools/widgets.hpp"

namespace devmode::frame_editors {

class AnchorFrameEditor : public FrameEditorBase {
public:
    AnchorFrameEditor() = default;

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
    void persist_changes();
    void apply_live_changes(bool force = false);
    void invalidate_preview() const;
    void refresh_selection_state();
    void update_selected_anchor_from_world(const SDL_FPoint& world_pos, float world_z);
    float parent_height_px() const;
    bool resolve_anchor_screen(int anchor_index, SDL_FPoint& out_screen, float& out_world_z, SDL_FPoint* out_world = nullptr) const;
    bool ui_contains_point(const SDL_Point& p) const;
    SDL_Rect anchor_list_rect() const;
    void render_anchor_list(SDL_Renderer* renderer) const;
    void rebuild_anchor_rows() const;
    void ensure_anchor_exists_everywhere(const std::string& name);
    void remove_anchor_everywhere(const std::string& name);
    void rename_anchor_everywhere(const std::string& old_name, const std::string& new_name);

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
    std::unique_ptr<DMButton> btn_apply_all_;
    std::unique_ptr<Point3DEditor> point_3d_editor_;

    std::unique_ptr<DMTextBox> tb_name_;
    std::unique_ptr<DMTextBox> tb_px_;
    std::unique_ptr<DMTextBox> tb_py_;
    std::unique_ptr<DMTextBox> tb_pz_;
    std::unique_ptr<DMTextBox> tb_rot_;

    mutable SDL_Rect ui_rect_{0, 0, 0, 0};
    mutable std::vector<SDL_Rect> anchor_rows_;
};

}  // namespace devmode::frame_editors
