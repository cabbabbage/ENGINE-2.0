#include "AnchorFrameEditor.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>

#include "animation/animation_update.hpp"
#include "devtools/dm_styles.hpp"
#include "devtools/draw_utils.hpp"
#include "devtools/font_cache.hpp"
#include "devtools/frame_editors/shared/SelectionState.hpp"
#include "devtools/widgets.hpp"
#include "rendering/render/warped_screen_grid.hpp"
#include "utils/AnchorPointResolver.hpp"
#include "utils/FramePointResolver.hpp"
#include "utils/sdl_mouse_utils.hpp"

namespace devmode::frame_editors {

namespace {

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

class AnchorListWidget : public Widget {
public:
    AnchorListWidget(const std::vector<AnchorFrame>* frames,
                     const int* selected_frame,
                     const int* selected_anchor,
                     std::function<void(int)> on_select)
        : frames_(frames),
          selected_frame_(selected_frame),
          selected_anchor_(selected_anchor),
          on_select_(std::move(on_select)) {}

    void set_rect(const SDL_Rect& r) override { rect_ = r; }
    const SDL_Rect& rect() const override { return rect_; }

    int height_for_width(int) const override {
        const int row_h = DMButton::height();
        const int count = anchor_count();
        return std::max(row_h + DMSpacing::small_gap() * 2,
                        count * (row_h + DMSpacing::small_gap()) + DMSpacing::small_gap() * 2);
    }

    bool handle_event(const SDL_Event& e) override {
        if (e.type != SDL_EVENT_MOUSE_BUTTON_DOWN || e.button.button != SDL_BUTTON_LEFT) {
            return false;
        }
        SDL_Point p{e.button.x, e.button.y};
        if (!SDL_PointInRect(&p, &rect_)) {
            return false;
        }
        const int row_h = DMButton::height();
        int cursor_y = rect_.y + DMSpacing::small_gap();
        const int count = anchor_count();
        for (int i = 0; i < count; ++i) {
            SDL_Rect row{rect_.x + DMSpacing::small_gap(),
                         cursor_y,
                         std::max(0, rect_.w - DMSpacing::small_gap() * 2),
                         row_h};
            if (SDL_PointInRect(&p, &row)) {
                if (on_select_) on_select_(i);
                return true;
            }
            cursor_y += row_h + DMSpacing::small_gap();
        }
        return true;
    }

    void render(SDL_Renderer* renderer) const override {
        if (!renderer) return;
        dm_draw::DrawBeveledRect(renderer,
                                 rect_,
                                 DMStyles::CornerRadius(),
                                 DMStyles::BevelDepth(),
                                 DMStyles::PanelBG(),
                                 DMStyles::PanelBG(),
                                 DMStyles::PanelBG(),
                                 false,
                                 0.0f,
                                 0.0f);
        const auto* anchors = anchor_list();
        if (!anchors) return;
        const int row_h = DMButton::height();
        int cursor_y = rect_.y + DMSpacing::small_gap();
        for (std::size_t i = 0; i < anchors->size(); ++i) {
            SDL_Rect row{rect_.x + DMSpacing::small_gap(),
                         cursor_y,
                         std::max(0, rect_.w - DMSpacing::small_gap() * 2),
                         row_h};
            const bool selected = selected_anchor_ && (*selected_anchor_ == static_cast<int>(i));
            SDL_Color bg = selected ? DMStyles::AccentButton().bg : DMStyles::ButtonBaseFill();
            bg.a = selected ? 240 : 210;
            dm_draw::DrawBeveledRect(renderer,
                                     row,
                                     DMStyles::CornerRadius(),
                                     DMStyles::BevelDepth(),
                                     bg,
                                     bg,
                                     bg,
                                     false,
                                     DMStyles::HighlightIntensity(),
                                     DMStyles::ShadowIntensity());
            const std::string label = (*anchors)[i].name.empty() ? "(unnamed)" : (*anchors)[i].name;
            DMFontCache::instance().draw_text(renderer,
                                              DMStyles::Label(),
                                              label,
                                              row.x + DMSpacing::small_gap(),
                                              row.y + (row.h - DMStyles::Label().font_size) / 2);
            cursor_y += row_h + DMSpacing::small_gap();
        }
    }

    bool wants_full_row() const override { return true; }
    void set_selected_anchor_ref(const int* ptr) { selected_anchor_ = ptr; }

private:
    const std::vector<FrameAnchorPoint>* anchor_list() const {
        if (!frames_ || !selected_frame_ || *selected_frame_ < 0 ||
            *selected_frame_ >= static_cast<int>(frames_->size())) {
            return nullptr;
        }
        return &frames_->at(static_cast<std::size_t>(*selected_frame_)).anchors;
    }

    int anchor_count() const {
        const auto* list = anchor_list();
        return list ? static_cast<int>(list->size()) : 0;
    }

    const std::vector<AnchorFrame>* frames_ = nullptr;
    const int* selected_frame_ = nullptr;
    const int* selected_anchor_ = nullptr;
    std::function<void(int)> on_select_;
    SDL_Rect rect_{0, 0, 0, DMButton::height()};
};

void AnchorListWidgetDeleter::operator()(AnchorListWidget* ptr) const noexcept {
    delete ptr;
}

AnchorFrameEditor::~AnchorFrameEditor() = default;

void AnchorFrameEditor::begin(const FrameEditorContext& context) {
    context_ = context;
    selection_state_ = context.selection_state;
    wants_close_ = false;
    selected_frame_ = 0;
    selected_anchor_ = -1;
    dirty_ = false;
    frames_.clear();
    screen_w_ = std::max(screen_w_, 1920);
    screen_h_ = std::max(screen_h_, 1080);

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
    frame_navigator_->set_on_before_change([this](int, int) {
        persist_pending_changes();
        return true;
    });
    frame_navigator_->set_on_apply_next([this]() { apply_to_next_frame(); });
    frame_navigator_->set_on_apply_animation([this]() { apply_to_animation(); });
    frame_navigator_->set_on_apply_all([this]() { (void)apply_to_all_animations(); });
    frame_navigator_->set_preview_source(context_.preview, context_.animation_id);

    btn_back_ = std::make_unique<DMButton>("Back", &DMStyles::HeaderButton(), 80, DMButton::height());
    btn_add_ = std::make_unique<DMButton>("Add Anchor", &DMStyles::AccentButton(), 140, DMButton::height());
    btn_delete_ = std::make_unique<DMButton>("Delete Anchor", &DMStyles::DeleteButton(), 140, DMButton::height());

    tb_name_ = std::make_unique<DMTextBox>("Name", "");
    tb_px_ = std::make_unique<DMTextBox>("px (percent of height)", "0.0");
    tb_py_ = std::make_unique<DMTextBox>("py (percent of height)", "0.0");
    tb_pz_ = std::make_unique<DMTextBox>("pz (percent of height)", "0.0");
    tb_rot_ = std::make_unique<DMTextBox>("rotation (deg)", "0.0");

    tool_panel_ = std::make_unique<FrameToolPanel>("Anchor Tool Panel", "frame_editor_tool_panel_anchor");
    back_widget_ = std::make_unique<ButtonWidget>(btn_back_.get(), [this]() { wants_close_ = true; });
    add_widget_ = std::make_unique<ButtonWidget>(btn_add_.get(), [this]() { add_anchor(); });
    delete_widget_ = std::make_unique<ButtonWidget>(btn_delete_.get(), [this]() {
        if (frames_.empty() || selected_frame_ < 0 || selected_frame_ >= static_cast<int>(frames_.size()) ||
            selected_anchor_ < 0) {
            return;
        }
        auto& frame = frames_.at(static_cast<std::size_t>(selected_frame_));
        if (selected_anchor_ >= static_cast<int>(frame.anchors.size())) {
            return;
        }
        frame.anchors.erase(frame.anchors.begin() + static_cast<std::size_t>(selected_anchor_));
        const int remaining = static_cast<int>(frame.anchors.size());
        select_anchor(std::min(selected_anchor_, remaining - 1));
        dirty_ = true;
    });
    anchor_list_widget_.reset(new AnchorListWidget(&frames_, &selected_frame_, &selected_anchor_, [this](int idx) {
        select_anchor(idx);
    }));
    anchor_list_widget_->set_selected_anchor_ref(&selected_anchor_);
    name_widget_ = std::make_unique<TextBoxWidget>(tb_name_.get(), true);
    px_widget_ = std::make_unique<TextBoxWidget>(tb_px_.get(), true);
    py_widget_ = std::make_unique<TextBoxWidget>(tb_py_.get(), true);
    pz_widget_ = std::make_unique<TextBoxWidget>(tb_pz_.get(), true);
    rot_widget_ = std::make_unique<TextBoxWidget>(tb_rot_.get(), true);
    rebuild_tool_panel_layout();
    tool_panel_->set_position_if_unset(DMSpacing::item_gap(), DMSpacing::header_gap());

    point_3d_editor_ = std::make_unique<Point3DEditor>(selection_state_);
    if (point_3d_editor_) {
        point_3d_editor_->reset_axis(AdjustmentAxis::X);
        point_3d_editor_->set_grid_resolution(context_.snap_resolution);
        point_3d_editor_->set_xy_display_mode(CoordinateDisplayMode::Percentage);
        point_3d_editor_->set_z_display_mode(CoordinateDisplayMode::Percentage);
        point_3d_editor_->set_parent_height(parent_height_px());

        point_3d_editor_->set_on_coordinates_changed([this]() {
            if (!selection_state_) return;
            update_selected_anchor_from_world(selection_state_->world_pos, selection_state_->world_z);
        });

        point_3d_editor_->set_on_position_changed([this](const SDL_FPoint& new_world_pos, float new_world_z) {
            update_selected_anchor_from_world(new_world_pos, new_world_z);
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
    tool_panel_.reset();
    back_widget_.reset();
    add_widget_.reset();
    delete_widget_.reset();
    anchor_list_widget_.reset();
    name_widget_.reset();
    px_widget_.reset();
    py_widget_.reset();
    pz_widget_.reset();
    rot_widget_.reset();
    frame_navigator_.reset();
    btn_back_.reset();
    btn_add_.reset();
    btn_delete_.reset();
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

    bool handled = false;

    if (tool_panel_) {
        const std::string name_before = tb_name_ ? tb_name_->value() : std::string{};
        const std::string px_before = tb_px_ ? tb_px_->value() : std::string{};
        const std::string py_before = tb_py_ ? tb_py_->value() : std::string{};
        const std::string pz_before = tb_pz_ ? tb_pz_->value() : std::string{};
        const std::string rot_before = tb_rot_ ? tb_rot_->value() : std::string{};

        if (tool_panel_->handle_event(e)) {
            handled = true;
        }

        const bool text_changed =
            (tb_name_ && tb_name_->value() != name_before) ||
            (tb_px_ && tb_px_->value() != px_before) ||
            (tb_py_ && tb_py_->value() != py_before) ||
            (tb_pz_ && tb_pz_->value() != pz_before) ||
            (tb_rot_ && tb_rot_->value() != rot_before);

        if (text_changed && apply_form_to_anchor()) {
            refresh_selection_state();
            dirty_ = true;
            handled = true;
        }
    }

    if (frame_navigator_ && frame_navigator_->handle_event(e)) {
        handled = true;
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

void AnchorFrameEditor::update(const Input& input, float) {
    nav_rect_.x = 0;
    nav_rect_.y = 0;
    nav_rect_.w = screen_w_;
    nav_rect_.h = frame_navigator_ ? frame_navigator_->get_preferred_rect().h : 0;
    if (frame_navigator_) {
        frame_navigator_->set_rect(nav_rect_);
    }
    if (tool_panel_) {
        tool_panel_->set_work_area(SDL_Rect{0, 0, screen_w_, screen_h_});
        tool_panel_->set_position_if_unset(DMSpacing::item_gap(), nav_rect_.h + DMSpacing::header_gap());
        tool_panel_->update(input, screen_w_, screen_h_);
    }
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
    if (frame_navigator_) frame_navigator_->render(renderer);
    if (tool_panel_) tool_panel_->render(renderer);

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
    int sw = screen_w_;
    int sh = screen_h_;
    if (renderer) {
        SDL_GetCurrentRenderOutputSize(renderer, &sw, &sh);
        screen_w_ = sw;
        screen_h_ = sh;
    }
    const int nav_height = frame_navigator_ ? frame_navigator_->get_preferred_rect().h : 0;
    nav_rect_.x = 0;
    nav_rect_.y = 0;
    nav_rect_.w = sw;
    nav_rect_.h = nav_height;
    if (frame_navigator_ && nav_rect_.w > 0) {
        frame_navigator_->set_rect(nav_rect_);
    }
    if (tool_panel_) {
        tool_panel_->set_work_area(SDL_Rect{0, 0, sw, sh});
        tool_panel_->set_position_if_unset(DMSpacing::item_gap(), nav_height + DMSpacing::header_gap());
    }
}

void AnchorFrameEditor::select_frame(int index) {
    apply_live_changes(true);
    selected_frame_ = std::clamp(index, 0, static_cast<int>(frames_.size()) - 1);
    selected_anchor_ = std::min(selected_anchor_, static_cast<int>(frames_[static_cast<std::size_t>(selected_frame_)].anchors.size()) - 1);
    refresh_form();
    refresh_selection_state();
    rebuild_tool_panel_layout();
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
    rebuild_tool_panel_layout();
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
        frames_[i] = source;
    }
    dirty_ = true;
}

void AnchorFrameEditor::apply_to_next_frame() {
    if (frames_.empty() || selected_frame_ < 0) return;
    const int count = static_cast<int>(frames_.size());
    const int current = std::clamp(selected_frame_, 0, count - 1);
    const int target = (current + 1) % count;
    selected_frame_ = current;
    frames_[static_cast<std::size_t>(target)] = frames_[static_cast<std::size_t>(selected_frame_)];
    dirty_ = true;
    persist_changes();
    persist_pending_changes();
}

void AnchorFrameEditor::apply_to_animation() {
    if (selected_frame_ < 0 || selected_frame_ >= static_cast<int>(frames_.size())) return;
    apply_to_all_frames();
    persist_changes();
    persist_pending_changes();
}

bool AnchorFrameEditor::apply_to_all_animations() {
    if (!context_.document || frames_.empty() || selected_frame_ < 0 || selected_frame_ >= static_cast<int>(frames_.size())) return false;
    apply_to_animation();

    const auto source = frames_[static_cast<std::size_t>(selected_frame_)];
    const auto ids = context_.document->animation_ids();
    for (const auto& id : ids) {
        auto payload_opt = context_.document->animation_payload_json(id);
        nlohmann::json payload = payload_opt.value_or(nlohmann::json::object());
        auto frames = parse_anchor_frames_from_payload(payload);
        if (frames.empty()) {
            frames.emplace_back();
        }
        for (auto& f : frames) {
            f = source;
        }
        nlohmann::json updated = build_payload_with_anchors(frames, payload);
        context_.document->update_animation_payload(id, updated);
        if (context_.preview) {
            context_.preview->invalidate(id);
        }
    }
    context_.document->save_to_file_checked(true);
    return true;
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
        DisplacedAssetAnchorPoint{anchor.name, anchor.px, anchor.py, anchor.pz, anchor.rotation},
        anchor_points::GridMaterialization::None);

    SDL_FPoint world_f{static_cast<float>(resolved.world_px.x), static_cast<float>(resolved.world_px.y)};
    out_world_z = static_cast<float>(resolved.world_z);
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
    if (SDL_PointInRect(&p, &nav_rect_)) return true;
    return tool_panel_ && tool_panel_->contains_point(p);
}

void AnchorFrameEditor::add_anchor() {
    if (frames_.empty() || selected_frame_ < 0) return;

    auto& frame = frames_.at(static_cast<std::size_t>(selected_frame_));
    int suffix = static_cast<int>(frame.anchors.size());
    auto name_exists = [&](const std::string& candidate) {
        for (const auto& a : frame.anchors) {
            if (a.name == candidate) return true;
        }
        return false;
    };
    std::string new_name;
    do {
        new_name = "anchor_" + std::to_string(suffix++);
    } while (name_exists(new_name));

    FrameAnchorPoint point;
    point.name = new_name;
    frame.anchors.push_back(point);
    select_anchor(static_cast<int>(frame.anchors.size()) - 1);
    dirty_ = true;
}

void AnchorFrameEditor::rebuild_tool_panel_layout() {
    if (!tool_panel_) return;
    DockableCollapsible::Rows rows;
    if (back_widget_) rows.push_back({back_widget_.get()});
    if (anchor_list_widget_) rows.push_back({anchor_list_widget_.get()});

    DockableCollapsible::Row buttons_row;
    if (add_widget_) buttons_row.push_back(add_widget_.get());
    if (delete_widget_) buttons_row.push_back(delete_widget_.get());
    if (!buttons_row.empty()) rows.push_back(buttons_row);

    if (selected_anchor_ >= 0) {
        if (name_widget_) rows.push_back({name_widget_.get()});
        if (px_widget_) rows.push_back({px_widget_.get()});
        if (py_widget_) rows.push_back({py_widget_.get()});
        if (pz_widget_) rows.push_back({pz_widget_.get()});
        if (rot_widget_) rows.push_back({rot_widget_.get()});
    }

    tool_panel_->set_rows(rows);
}

}  // namespace devmode::frame_editors
