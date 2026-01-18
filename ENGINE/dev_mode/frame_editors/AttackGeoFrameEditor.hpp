#pragma once

#include <array>
#include <memory>
#include <string>
#include <vector>

#include "FrameEditorBase.hpp"
#include "shared/AxisAdjuster.hpp"
#include "shared/FrameEditState.hpp"
#include "shared/FrameEditorContext.hpp"
#include "shared/ManifestTransaction.hpp"
#include "shared/SelectionState.hpp"
#include "dev_mode/widgets.hpp"

namespace devmode::frame_editors {

class AttackGeoFrameEditor : public FrameEditorBase {
public:
    void begin(const FrameEditorContext& context) override;
    void end() override;
    bool handle_event(const SDL_Event& e) override;
    void update(const Input& input, float dt) override;
    void render_world(SDL_Renderer* renderer) const override;
    void render_overlays(SDL_Renderer* renderer) const override;

private:
    enum class AttackHandle { None, Start, Control, End, Segment };

    void layout_ui(SDL_Renderer* renderer) const;
    void select_frame(int index);
    void clamp_attack_selection();
    void refresh_attack_form() const;
    void apply_text_fields();
    void persist_changes();
    void apply_attack_to_all_frames();
    void copy_attack_vector_to_next_frame();
    std::string current_attack_type() const;
    int current_attack_vector_index() const;
    void set_current_attack_vector_index(int index);
    animation_update::FrameAttackGeometry::Vector* current_attack_vector();
    const animation_update::FrameAttackGeometry::Vector* current_attack_vector() const;
    animation_update::FrameAttackGeometry::Vector* ensure_attack_vector_for_type(const std::string& type);
    void delete_current_attack_vector();
    bool begin_attack_drag(SDL_Point mouse);
    void update_attack_drag(SDL_Point mouse);
    void end_attack_drag(bool commit);
    void render_attack_geometry(SDL_Renderer* renderer) const;
    SDL_Point asset_anchor_world() const;
    float asset_local_scale() const;
    bool screen_to_local(SDL_Point screen, SDL_FPoint& out_local) const;
    void refresh_selection_state();
    bool ui_contains_point(const SDL_Point& p) const;

    FrameEditorContext context_{};
    SelectionState* selection_state_ = nullptr;
    AxisAdjuster* axis_adjuster_ = nullptr;
    ManifestTransaction manifest_txn_;
    std::vector<MovementFrame> frames_;
    int selected_index_ = 0;
    int selected_attack_type_index_ = 1;
    std::array<int, kDamageTypeNames.size()> selected_attack_vector_indices_{{-1, -1, -1}};

    std::unique_ptr<DMDropdown> dd_attack_type_;
    std::unique_ptr<DMButton> btn_prev_frame_;
    std::unique_ptr<DMButton> btn_next_frame_;
    std::unique_ptr<DMButton> btn_add_remove_;
    std::unique_ptr<DMButton> btn_delete_;
    std::unique_ptr<DMButton> btn_copy_next_;
    std::unique_ptr<DMButton> btn_apply_all_;
    std::unique_ptr<DMTextBox> tb_start_x_;
    std::unique_ptr<DMTextBox> tb_start_y_;
    std::unique_ptr<DMTextBox> tb_control_x_;
    std::unique_ptr<DMTextBox> tb_control_y_;
    std::unique_ptr<DMTextBox> tb_end_x_;
    std::unique_ptr<DMTextBox> tb_end_y_;
    std::unique_ptr<DMTextBox> tb_damage_;
    mutable std::string last_start_x_{};
    mutable std::string last_start_y_{};
    mutable std::string last_control_x_{};
    mutable std::string last_control_y_{};
    mutable std::string last_end_x_{};
    mutable std::string last_end_y_{};
    mutable std::string last_damage_{};

    mutable SDL_Rect ui_rect_{0, 0, 0, 0};

    AttackHandle active_handle_ = AttackHandle::None;
    bool dragging_attack_ = false;
    bool drag_moved_ = false;
    SDL_Point drag_start_mouse_{0, 0};
    SDL_FPoint drag_start_mouse_local_{0.0f, 0.0f};
    animation_update::FrameAttackGeometry::Vector drag_start_vector_{};
};

}  // namespace devmode::frame_editors
