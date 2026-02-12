#include "AnchorFrameEditor.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>

#include "animation/animation_update.hpp"
#include "devtools/dm_styles.hpp"
#include "devtools/draw_utils.hpp"
#include "devtools/font_cache.hpp"
#include "devtools/frame_editors/shared/SelectionState.hpp"
#include "devtools/frame_editors/shared/SnapUtils.hpp"
#include "devtools/widgets.hpp"
#include "rendering/render/warped_screen_grid.hpp"
#include "utils/AnchorPointResolver.hpp"
#include "utils/FramePointResolver.hpp"
#include "utils/sdl_mouse_utils.hpp"

namespace devmode::frame_editors {

namespace {

constexpr int kPanelPadding = 12;
constexpr int kListWidth = 240;
constexpr int kControlsWidth = 260;
constexpr SDL_Color kListBg{24, 26, 32, 220};
constexpr SDL_Color kListAccent{90, 200, 255, 255};

float parse_float_box(DMTextBox* box, float fallback = 0.0f) {
    if (!box) return fallback;
    try {
        return std::stof(box->value());
    } catch (...) {
        return fallback;
    }
}

SDL_Point round_point(const SDL_FPoint& pt) {
    return SDL_Point{static_cast<int>(std::lround(pt.x)), static_cast<int>(std::lround(pt.y))};
}

}  // namespace

void AnchorFrameEditor::begin(const FrameEditorContext& context) {
    context_ = context;
    selection_state_ = context.selection_state;
    wants_close_ = false;
    selected_frame_ = 0;
    selected_anchor_ = -1;
    dirty_ = false;
    frames_.clear();
    anchor_rows_.clear();

    if (selection_state_) {
        selection_state_->reset();
    }

    if (context_.document) {
        auto payload_opt = context_.document->animation_payload_json(context_.animation_id);
        nlohmann::json payload = payload_opt.value_or(nlohmann::json::object());
        frames_ = parse_anchor_frames_from_payload(payload);
        if (frames_.empty()) {
            frames_.resize(1);
        }
    } else {
        frames_.resize(1);
    }

    frame_navigator_ = std::make_unique<FrameNavigator>();
    frame_navigator_->set_frame_count(static_cast<int>(frames_.size()));
    frame_navigator_->set_current_frame(selected_frame_);
    frame_navigator_->set_on_frame_changed([this](int idx) { select_frame(idx); });

    btn_back_ = std::make_unique<DMButton>("Back", &DMStyles::HeaderButton(), 80, DMButton::height());
    btn_add_ = std::make_unique<DMButton>("Add Anchor", &DMStyles::AccentButton(), 140, DMButton::height());
    btn_delete_ = std::make_unique<DMButton>("Delete Anchor", &DMStyles::DeleteButton(), 140, DMButton::height());
    btn_apply_all_ = std::make_unique<DMButton>("Apply To All Frames", &DMStyles::HeaderButton(), 180, DMButton::height());

    tb_name_ = std::make_unique<DMTextBox>("Name", "");
    tb_px_ = std::make_unique<DMTextBox>("px (percent of height)", "0.0");
    tb_py_ = std::make_unique<DMTextBox>("py (percent of height)", "0.0");
    tb_pz_ = std::make_unique<DMTextBox>("pz (percent of height)", "0.0");
    tb_rot_ = std::make_unique<DMTextBox>("rotation (deg)", "0.0");

    point_3d_editor_ = std::make_unique<Point3DEditor>(selection_state_);
    if (point_3d_editor_) {
        point_3d_editor_->reset_axis(AdjustmentAxis::X);
        point_3d_editor_->set_grid_resolution(context_.snap_resolution);
        point_3d_editor_->set_xy_display_mode(CoordinateDisplayMode::Percentage);
        point_3d_editor_->set_z_display_mode(CoordinateDisplayMode::Percentage);
        point_3d_editor_->set_parent_height(parent_height_px());

        point_3d_editor_->set_on_coordinates_changed([this]() {
            if (!selection_state_) return;
            SDL_FPoint snapped_world = snap_world_point_to_grid(selection_state_->world_pos, context_.snap_resolution);
            float snapped_world_z = snap_world_z_to_grid(selection_state_->world_z, context_.snap_resolution);
            selection_state_->world_pos = snapped_world;
            selection_state_->world_z = snapped_world_z;
            update_selected_anchor_from_world(snapped_world, snapped_world_z);
        });

        point_3d_editor_->set_on_position_changed([this](const SDL_FPoint& new_world_pos, float new_world_z) {
            SDL_FPoint snapped_world = snap_world_point_to_grid(new_world_pos, context_.snap_resolution);
            float snapped_world_z = snap_world_z_to_grid(new_world_z, context_.snap_resolution);
            update_selected_anchor_from_world(snapped_world, snapped_world_z);
        });

        point_3d_editor_->set_on_point_selected([this](int index) {
            if (index < 0) {
                if (selection_state_) selection_state_->reset();
                return;
            }
            select_anchor(index);
        });
    }

    refresh_form();
    refresh_selection_state();

    manifest_txn_.begin(context_);
    manifest_txn_.set_immediate_persist(false);
    manifest_txn_.set_apply_callback([this]() {
        persist_changes();
        return true;
    });
}

void AnchorFrameEditor::end() {
    frames_.clear();
    anchor_rows_.clear();
    frame_navigator_.reset();
    btn_back_.reset();
    btn_add_.reset();
    btn_delete_.reset();
    btn_apply_all_.reset();
    point_3d_editor_.reset();
    if (selection_state_) {
        selection_state_->reset();
        selection_state_ = nullptr;
    }
    tb_name_.reset();
    tb_px_.reset();
    tb_py_.reset();
    tb_pz_.reset();
    tb_rot_.reset();
    dirty_ = false;
    wants_close_ = false;
}

bool AnchorFrameEditor::handle_event(const SDL_Event& e) {
    SDL_Rect overlay_rect{0, 0, 0, 0};
    bool overlay_valid = false;
    if (point_3d_editor_) {
        overlay_rect = point_3d_editor_->get_cached_container();
        overlay_valid = (overlay_rect.w > 0 && overlay_rect.h > 0);
        if (point_3d_editor_->handle_event(e, overlay_rect)) {
            return true;
        }
    }

    auto anchor_index_for_name = [&](const std::string& name) -> int {
        const auto& frame = frames_.at(static_cast<std::size_t>(selected_frame_));
        for (std::size_t i = 0; i < frame.anchors.size(); ++i) {
            if (frame.anchors[i].name == name) {
                return static_cast<int>(i);
            }
        }
        return -1;
    };

    bool handled = false;
    if (frame_navigator_ && frame_navigator_->handle_event(e)) {
        select_frame(frame_navigator_->get_current_frame());
        handled = true;
    }
    if (btn_back_ && btn_back_->handle_event(e)) {
        wants_close_ = true;
        handled = true;
    }
    if (btn_add_ && btn_add_->handle_event(e)) {
        // Generate a unique anchor name and add it everywhere (all frames, all animations)
        int suffix = static_cast<int>(frames_.at(static_cast<std::size_t>(selected_frame_)).anchors.size());
        auto name_exists = [&](const std::string& candidate) {
            for (const auto& f : frames_) {
                for (const auto& a : f.anchors) {
                    if (a.name == candidate) return true;
                }
            }
            return false;
        };
        std::string new_name;
        do {
            new_name = "anchor_" + std::to_string(suffix++);
        } while (name_exists(new_name));

        ensure_anchor_exists_everywhere(new_name);
        select_anchor(anchor_index_for_name(new_name));
        dirty_ = true;
        handled = true;
    }
    if (btn_delete_ && btn_delete_->handle_event(e)) {
        const auto& frame = frames_.at(static_cast<std::size_t>(selected_frame_));
        if (selected_anchor_ >= 0 && selected_anchor_ < static_cast<int>(frame.anchors.size())) {
            const std::string name = frame.anchors[static_cast<std::size_t>(selected_anchor_)].name;
            remove_anchor_everywhere(name);
            const int remaining = static_cast<int>(frames_.at(static_cast<std::size_t>(selected_frame_)).anchors.size());
            select_anchor(std::min(selected_anchor_, remaining - 1));
            dirty_ = true;
        }
        handled = true;
    }
    if (btn_apply_all_ && btn_apply_all_->handle_event(e)) {
        apply_to_all_frames();
        handled = true;
    }

    bool text_changed = false;
    if (tb_name_ && tb_name_->handle_event(e)) {
        text_changed = true;
        handled = true;
    }
    if (tb_px_ && tb_px_->handle_event(e)) {
        text_changed = true;
        handled = true;
    }
    if (tb_py_ && tb_py_->handle_event(e)) {
        text_changed = true;
        handled = true;
    }
    if (tb_pz_ && tb_pz_->handle_event(e)) {
        text_changed = true;
        handled = true;
    }
    if (tb_rot_ && tb_rot_->handle_event(e)) {
        text_changed = true;
        handled = true;
    }

    if (text_changed) {
        if (apply_form_to_anchor()) {
            refresh_selection_state();
            dirty_ = true;
        }
    }

    if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
        SDL_Point p{e.button.x, e.button.y};
        for (std::size_t i = 0; i < anchor_rows_.size(); ++i) {
            if (SDL_PointInRect(&p, &anchor_rows_[i])) {
                select_anchor(static_cast<int>(i));
                handled = true;
                break;
            }
        }
    }

    if (!context_.assets || !context_.target || !point_3d_editor_) {
        return handled;
    }

    SDL_Point mouse_pos{0, 0};
    if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_UP) {
        mouse_pos = {static_cast<int>(std::lround(e.button.x)), static_cast<int>(std::lround(e.button.y))};
    } else if (e.type == SDL_EVENT_MOUSE_MOTION) {
        mouse_pos = {static_cast<int>(std::lround(e.motion.x)), static_cast<int>(std::lround(e.motion.y))};
    } else if (e.type == SDL_EVENT_MOUSE_WHEEL) {
        sdl_mouse_util::GetMouseState(&mouse_pos.x, &mouse_pos.y);
    }

    const bool pointer_in_overlay = overlay_valid && SDL_PointInRect(&mouse_pos, &overlay_rect);
    if (!ui_contains_point(mouse_pos) && !pointer_in_overlay) {
        std::vector<SDL_FPoint> point_screens;
        std::vector<bool> selectable;
        const auto& frame = frames_.at(static_cast<std::size_t>(selected_frame_));
        point_screens.reserve(frame.anchors.size());
        selectable.reserve(frame.anchors.size());
        for (std::size_t i = 0; i < frame.anchors.size(); ++i) {
            SDL_FPoint screen{};
            float world_z = 0.0f;
            if (resolve_anchor_screen(static_cast<int>(i), screen, world_z)) {
                point_screens.push_back(screen);
                selectable.push_back(true);
            }
        }
        if (!point_screens.empty() && point_3d_editor_->handle_mouse_event(e, point_screens, selectable)) {
            handled = true;
        }
    }

    return handled;
}

void AnchorFrameEditor::update(const Input&, float) {
    layout_ui();
    refresh_selection_state();
}

void AnchorFrameEditor::render_world(SDL_Renderer*) const {
    // No 3D world rendering for this editor.
}

void AnchorFrameEditor::render_overlays(SDL_Renderer* renderer) const {
    if (!renderer || !context_.target) {
        return;
    }

    layout_ui(renderer);

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    // Anchor list container
    SDL_Rect list_rect = anchor_list_rect();
    dm_draw::DrawBeveledRect(renderer,
                             list_rect,
                             DMStyles::CornerRadius(),
                             DMStyles::BevelDepth(),
                             kListBg,
                             kListBg,
                             kListBg,
                             false,
                             0.0f,
                             0.0f);
    render_anchor_list(renderer);

    // Controls container background
    SDL_Rect controls_bg{list_rect.x + list_rect.w + DMSpacing::item_gap(),
                         list_rect.y,
                         kControlsWidth,
                         ui_rect_.h};
    dm_draw::DrawBeveledRect(renderer,
                             controls_bg,
                             DMStyles::CornerRadius(),
                             DMStyles::BevelDepth(),
                             SDL_Color{18, 20, 26, 200},
                             SDL_Color{18, 20, 26, 200},
                             SDL_Color{18, 20, 26, 200},
                             false,
                             0.0f,
                             0.0f);

    if (frame_navigator_) frame_navigator_->render(renderer);
    if (btn_back_) btn_back_->render(renderer);
    if (btn_add_) btn_add_->render(renderer);
    if (btn_delete_) btn_delete_->render(renderer);
    if (btn_apply_all_) btn_apply_all_->render(renderer);
    if (tb_name_) tb_name_->render(renderer);
    if (tb_px_) tb_px_->render(renderer);
    if (tb_py_) tb_py_->render(renderer);
    if (tb_pz_) tb_pz_->render(renderer);
    if (tb_rot_) tb_rot_->render(renderer);

    const auto& frame = frames_.at(static_cast<std::size_t>(selected_frame_));
    for (std::size_t i = 0; i < frame.anchors.size(); ++i) {
        SDL_FPoint screen{};
        float world_z = 0.0f;
        SDL_FPoint world{};
        if (!resolve_anchor_screen(static_cast<int>(i), screen, world_z, &world)) {
            continue;
        }
        const bool is_selected = static_cast<int>(i) == selected_anchor_;
        const bool is_hovered = point_3d_editor_ && static_cast<int>(i) == point_3d_editor_->get_hovered_point_index();
        if (point_3d_editor_) {
            point_3d_editor_->render_selectable_point(renderer, screen, is_selected, is_hovered, 7.0f);
        } else {
            SDL_Color color = is_selected ? kListAccent : SDL_Color{200, 200, 200, 255};
            SDL_Rect dot{
                static_cast<int>(std::lround(screen.x)) - 4,
                static_cast<int>(std::lround(screen.y)) - 4,
                8,
                8};
            dm_draw::DrawRoundedSolidRect(renderer, dot, 4, color);
        }
    }

    if (point_3d_editor_) {
        int sw = 0, sh = 0;
        SDL_GetCurrentRenderOutputSize(renderer, &sw, &sh);
        int height = point_3d_editor_->get_overlay_height(sw);
        SDL_Rect bottom_container{0, sh - height, sw, height};
        point_3d_editor_->render_overlays(renderer, bottom_container);
    }
}

void AnchorFrameEditor::persist_pending_changes() {
    if (!manifest_txn_.active() || !dirty_) {
        return;
    }
    if (manifest_txn_.commit(true)) {
        dirty_ = false;
    }
}

void AnchorFrameEditor::layout_ui(SDL_Renderer* renderer) const {
    const int padding = kPanelPadding;
    const int left_x = padding;
    const int top_y = padding;
    const int controls_x = left_x + kListWidth + DMSpacing::item_gap();
    int cursor_y = top_y + padding;

    ui_rect_ = SDL_Rect{left_x, top_y, kListWidth + kControlsWidth + DMSpacing::item_gap(), 0};

    if (frame_navigator_) {
        SDL_Rect preferred = frame_navigator_->get_preferred_rect();
        SDL_Rect rect{controls_x, cursor_y, kControlsWidth - padding * 2, preferred.h};
        frame_navigator_->set_rect(rect);
        cursor_y += rect.h + padding;
    }
    if (btn_back_) {
        btn_back_->set_rect(SDL_Rect{controls_x, cursor_y, kControlsWidth - padding * 2, DMButton::height()});
        cursor_y += DMButton::height() + padding;
    }
    if (btn_add_) {
        btn_add_->set_rect(SDL_Rect{controls_x, cursor_y, kControlsWidth - padding * 2, DMButton::height()});
        cursor_y += DMButton::height() + padding;
    }
    if (btn_delete_) {
        btn_delete_->set_rect(SDL_Rect{controls_x, cursor_y, kControlsWidth - padding * 2, DMButton::height()});
        cursor_y += DMButton::height() + padding;
    }
    if (btn_apply_all_) {
        btn_apply_all_->set_rect(SDL_Rect{controls_x, cursor_y, kControlsWidth - padding * 2, DMButton::height()});
        cursor_y += DMButton::height() + padding;
    }
    auto place_textbox = [&](DMTextBox* tb) {
        if (!tb) return;
        tb->set_rect(SDL_Rect{controls_x, cursor_y, kControlsWidth - padding * 2, DMTextBox::height()});
        cursor_y += DMTextBox::height() + padding;
    };
    place_textbox(tb_name_.get());
    place_textbox(tb_px_.get());
    place_textbox(tb_py_.get());
    place_textbox(tb_pz_.get());
    place_textbox(tb_rot_.get());

    const int controls_height = cursor_y - top_y;
    const int list_height = anchor_list_rect().h + padding * 2;
    const int panel_height = std::max(list_height, controls_height);
    ui_rect_.h = panel_height;

    rebuild_anchor_rows();
    (void)renderer;
}

void AnchorFrameEditor::select_frame(int index) {
    apply_live_changes(true);
    selected_frame_ = std::clamp(index, 0, static_cast<int>(frames_.size()) - 1);
    selected_anchor_ = std::min(selected_anchor_, static_cast<int>(frames_[static_cast<std::size_t>(selected_frame_)].anchors.size()) - 1);
    refresh_form();
    refresh_selection_state();
    if (frame_navigator_) {
        frame_navigator_->set_current_frame(selected_frame_);
    }
}

void AnchorFrameEditor::select_anchor(int index) {
    const auto& frame = frames_.at(static_cast<std::size_t>(selected_frame_));
    if (frame.anchors.empty()) {
        selected_anchor_ = -1;
    } else {
        selected_anchor_ = std::clamp(index, 0, static_cast<int>(frame.anchors.size()) - 1);
    }
    refresh_form();
    refresh_selection_state();
}

void AnchorFrameEditor::refresh_form() {
    if (!tb_name_) return;
    const auto& frame = frames_.at(static_cast<std::size_t>(selected_frame_));
    if (selected_anchor_ >= 0 && selected_anchor_ < static_cast<int>(frame.anchors.size())) {
        const auto& a = frame.anchors[static_cast<std::size_t>(selected_anchor_)];
        tb_name_->set_value(a.name);
        tb_px_->set_value(std::to_string(a.px));
        tb_py_->set_value(std::to_string(a.py));
        tb_pz_->set_value(std::to_string(a.pz));
        tb_rot_->set_value(std::to_string(a.rotation));
    } else {
        tb_name_->set_value("");
        tb_px_->set_value("0.0");
        tb_py_->set_value("0.0");
        tb_pz_->set_value("0.0");
        tb_rot_->set_value("0.0");
    }
}

bool AnchorFrameEditor::apply_form_to_anchor() {
    if (selected_anchor_ < 0) {
        return false;
    }
    auto& frame = frames_.at(static_cast<std::size_t>(selected_frame_));
    if (selected_anchor_ >= static_cast<int>(frame.anchors.size())) {
        return false;
    }
    auto& a = frame.anchors[static_cast<std::size_t>(selected_anchor_)];

    bool changed = false;
    const std::string old_name = a.name;
    if (tb_name_) {
        std::string requested_name = tb_name_->value();
        if (requested_name.empty()) {
            requested_name = old_name;
        }
        bool duplicate = false;
        for (std::size_t i = 0; i < frame.anchors.size(); ++i) {
            if (static_cast<int>(i) == selected_anchor_) continue;
            if (frame.anchors[i].name == requested_name) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate && requested_name != old_name) {
            rename_anchor_everywhere(old_name, requested_name);
            a.name = requested_name;
            changed = true;
        } else if (duplicate && tb_name_) {
            tb_name_->set_value(old_name);
        }
    }

    const float new_px = parse_float_box(tb_px_.get(), a.px);
    const float new_py = parse_float_box(tb_py_.get(), a.py);
    const float new_pz = std::clamp(parse_float_box(tb_pz_.get(), a.pz), 0.0f, 1.0f);
    const float new_rot = parse_float_box(tb_rot_.get(), a.rotation);

    if (a.px != new_px) {
        a.px = new_px;
        changed = true;
    }
    if (a.py != new_py) {
        a.py = new_py;
        changed = true;
    }
    if (a.pz != new_pz) {
        a.pz = new_pz;
        changed = true;
    }
    if (a.rotation != new_rot) {
        a.rotation = new_rot;
        changed = true;
    }

    return changed;
}

void AnchorFrameEditor::apply_to_all_frames() {
    if (frames_.empty()) return;
    if (selected_frame_ < 0 || selected_frame_ >= static_cast<int>(frames_.size())) return;
    const auto source = frames_[static_cast<std::size_t>(selected_frame_)];
    for (std::size_t i = 0; i < frames_.size(); ++i) {
        if (static_cast<int>(i) == selected_frame_) continue;
        frames_[i] = source;
    }
    dirty_ = true;
}

void AnchorFrameEditor::persist_changes() {
    if (!context_.document) {
        return;
    }
    apply_form_to_anchor();
    auto payload_opt = context_.document->animation_payload_json(context_.animation_id);
    nlohmann::json existing = payload_opt.value_or(nlohmann::json::object());
    nlohmann::json updated = build_payload_with_anchors(frames_, existing);
    context_.document->update_animation_payload(context_.animation_id, updated);
    invalidate_preview();
}

void AnchorFrameEditor::apply_live_changes(bool force) {
    if (!force) {
        return;
    }
    persist_pending_changes();
}

void AnchorFrameEditor::invalidate_preview() const {
    if (context_.preview && !context_.animation_id.empty()) {
        context_.preview->invalidate(context_.animation_id);
    }
}

void AnchorFrameEditor::refresh_selection_state() {
    if (!selection_state_ || !context_.target) {
        return;
    }
    if (selected_anchor_ < 0 || selected_anchor_ >= static_cast<int>(frames_.at(static_cast<std::size_t>(selected_frame_)).anchors.size())) {
        selection_state_->target = SelectionTarget::None;
        selection_state_->clear_anchor_world();
        return;
    }

    SDL_FPoint screen{};
    float world_z = 0.0f;
    SDL_FPoint world{};
    if (!resolve_anchor_screen(selected_anchor_, screen, world_z, &world)) {
        return;
    }

    FramePointResolver resolver(context_.target);
    selection_state_->target = SelectionTarget::AnchorPoint;
    selection_state_->world_pos = world;
    selection_state_->world_z = world_z;
    selection_state_->screen_pos = round_point(screen);
    selection_state_->set_anchor_world(resolver.anchor_world(), resolver.base_world_z());

    if (point_3d_editor_) {
        point_3d_editor_->set_parent_height(parent_height_px());
    }
}

void AnchorFrameEditor::update_selected_anchor_from_world(const SDL_FPoint& world_pos, float world_z) {
    if (!context_.target) return;
    if (selected_anchor_ < 0 || selected_anchor_ >= static_cast<int>(frames_.at(static_cast<std::size_t>(selected_frame_)).anchors.size())) {
        return;
    }

    const float height = parent_height_px();
    if (!(height > 0.0f)) {
        return;
    }

    SDL_Point base = animation_update::detail::bottom_middle_for(*context_.target, context_.target->world_point());
    const float base_z = static_cast<float>(context_.target->world_z());
    auto& anchor = frames_[static_cast<std::size_t>(selected_frame_)].anchors[static_cast<std::size_t>(selected_anchor_)];

    const float next_px = (world_pos.x - static_cast<float>(base.x)) / height;
    const float next_py = (world_pos.y - static_cast<float>(base.y)) / height;
    const float next_pz = std::clamp((world_z - base_z) / height, 0.0f, 1.0f);

    bool changed = false;
    if (anchor.px != next_px) {
        anchor.px = next_px;
        changed = true;
    }
    if (anchor.py != next_py) {
        anchor.py = next_py;
        changed = true;
    }
    if (anchor.pz != next_pz) {
        anchor.pz = next_pz;
        changed = true;
    }

    if (changed) {
        refresh_form();
        refresh_selection_state();
        dirty_ = true;
    }
}

float AnchorFrameEditor::parent_height_px() const {
    if (!context_.target) {
        return 0.0f;
    }
    const float runtime_height = context_.target->runtime_height_px();
    if (std::isfinite(runtime_height) && runtime_height > 0.0f) {
        return runtime_height;
    }
    FramePointResolver resolver(context_.target);
    return resolver.parent_height_px();
}

bool AnchorFrameEditor::resolve_anchor_screen(int anchor_index,
                                              SDL_FPoint& out_screen,
                                              float& out_world_z,
                                              SDL_FPoint* out_world) const {
    if (!context_.target || !context_.assets) {
        return false;
    }
    if (anchor_index < 0 || selected_frame_ < 0 || selected_frame_ >= static_cast<int>(frames_.size())) {
        return false;
    }
    const auto& frame = frames_.at(static_cast<std::size_t>(selected_frame_));
    if (anchor_index >= static_cast<int>(frame.anchors.size())) {
        return false;
    }
    const auto& anchor = frame.anchors[static_cast<std::size_t>(anchor_index)];
    const auto resolved = anchor_points::resolve_anchor_point(
        *context_.target,
        DisplacedAssetAnchorPoint{anchor.name, anchor.px, anchor.py, anchor.pz, anchor.rotation});

    SDL_FPoint world_f{static_cast<float>(resolved.world_px.x), static_cast<float>(resolved.world_px.y)};
    out_world_z = resolved.grid_point ? static_cast<float>(resolved.grid_point->world_z()) : static_cast<float>(context_.target->world_z());
    if (out_world) {
        *out_world = world_f;
    }

    const WarpedScreenGrid& cam = context_.camera ? *context_.camera : context_.assets->getView();
    SDL_FPoint projected{};
    if (context_.camera && context_.camera->project_world_point(world_f, out_world_z, projected)) {
        out_screen = projected;
    } else {
        out_screen = cam.map_to_screen_f(world_f);
    }
    return true;
}

bool AnchorFrameEditor::ui_contains_point(const SDL_Point& p) const {
    return SDL_PointInRect(&p, &ui_rect_);
}

SDL_Rect AnchorFrameEditor::anchor_list_rect() const {
    const int row_h = DMButton::height();
    const int row_count = frames_.empty() ? 0 : static_cast<int>(frames_[static_cast<std::size_t>(selected_frame_)].anchors.size());
    const int h = std::max(row_h + DMSpacing::small_gap() * 2, row_count * (row_h + DMSpacing::small_gap()) + DMSpacing::small_gap() * 2);
    return SDL_Rect{kPanelPadding, kPanelPadding, kListWidth, h};
}

void AnchorFrameEditor::render_anchor_list(SDL_Renderer* renderer) const {
    rebuild_anchor_rows();
    const auto& frame = frames_.at(static_cast<std::size_t>(selected_frame_));
    for (std::size_t i = 0; i < anchor_rows_.size(); ++i) {
        SDL_Rect row = anchor_rows_[i];
        SDL_Color bg = (static_cast<int>(i) == selected_anchor_) ? kListAccent : SDL_Color{60, 60, 60, 200};
        dm_draw::DrawBeveledRect(renderer, row, DMStyles::CornerRadius(), DMStyles::BevelDepth(), bg, bg, bg, false, 0.0f, 0.0f);
        const std::string label = frame.anchors[i].name.empty() ? "(unnamed)" : frame.anchors[i].name;
        DMFontCache::instance().draw_text(renderer,
                                          DMStyles::Label(),
                                          label,
                                          row.x + DMSpacing::small_gap(),
                                          row.y + (row.h - DMStyles::Label().font_size) / 2);
    }
}

void AnchorFrameEditor::rebuild_anchor_rows() const {
    anchor_rows_.clear();
    if (frames_.empty()) return;
    const auto& frame = frames_.at(static_cast<std::size_t>(selected_frame_));
    SDL_Rect list = anchor_list_rect();
    int cursor_y = list.y + DMSpacing::small_gap();
    const int row_h = DMButton::height();
    for (std::size_t i = 0; i < frame.anchors.size(); ++i) {
        SDL_Rect row{list.x + DMSpacing::small_gap(), cursor_y, list.w - DMSpacing::small_gap() * 2, row_h};
        anchor_rows_.push_back(row);
        cursor_y += row_h + DMSpacing::small_gap();
    }
}

void AnchorFrameEditor::ensure_anchor_exists_everywhere(const std::string& name) {
    if (name.empty()) return;

    for (auto& f : frames_) {
        const bool exists = std::any_of(f.anchors.begin(), f.anchors.end(), [&](const FrameAnchorPoint& a) { return a.name == name; });
        if (!exists) {
            FrameAnchorPoint pt;
            pt.name = name;
            f.anchors.push_back(pt);
        }
    }
}

void AnchorFrameEditor::remove_anchor_everywhere(const std::string& name) {
    if (name.empty()) return;
    for (auto& f : frames_) {
        f.anchors.erase(std::remove_if(f.anchors.begin(),
                                       f.anchors.end(),
                                       [&](const FrameAnchorPoint& a) { return a.name == name; }),
                        f.anchors.end());
    }
}

void AnchorFrameEditor::rename_anchor_everywhere(const std::string& old_name, const std::string& new_name) {
    if (old_name.empty() || new_name.empty() || old_name == new_name) {
        return;
    }

    for (auto& f : frames_) {
        for (auto& a : f.anchors) {
            if (a.name == old_name) {
                a.name = new_name;
            }
        }
    }
}

}  // namespace devmode::frame_editors
