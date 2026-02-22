#include "AnchorFrameEditor.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <unordered_set>

#include "animation/animation_update.hpp"
#include "devtools/dm_styles.hpp"
#include "devtools/draw_utils.hpp"
#include "devtools/font_cache.hpp"
#include "devtools/frame_editors/shared/SelectionState.hpp"
#include "devtools/widgets.hpp"
#include "assets/Asset.hpp"
#include "assets/animation_frame.hpp"
#include "assets/animation_frame_variant.hpp"
#include "assets/asset/animation.hpp"
#include "utils/AnchorPointResolver.hpp"

namespace devmode::frame_editors {

namespace {

constexpr SDL_Color kListAccent{90, 200, 255, 255};

int parse_int_box(DMTextBox* box, int fallback = 0) {
    if (!box) return fallback;
    try {
        return std::stoi(box->value());
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
        SDL_Point p{static_cast<int>(std::lround(e.button.x)),
                    static_cast<int>(std::lround(e.button.y))};
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
    target_frame_lock_active_ = false;
    saved_target_animation_.clear();
    saved_target_frame_ = nullptr;
    saved_target_static_frame_ = false;

    if (context_.target) {
        target_frame_lock_active_ = true;
        saved_target_animation_ = context_.target->current_animation;
        saved_target_frame_ = context_.target->current_frame;
        saved_target_static_frame_ = context_.target->static_frame;
    }

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
    if (!frames_.empty() && !frames_.front().anchors.empty()) {
        selected_anchor_ = 0;
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
    btn_save_ = std::make_unique<DMButton>("Save", &DMStyles::AccentButton(), 140, DMButton::height());

    tb_name_ = std::make_unique<DMTextBox>("Name", "");
    tb_tex_x_ = std::make_unique<DMTextBox>("Texture X (px)", "0");
    tb_tex_y_ = std::make_unique<DMTextBox>("Texture Y (px)", "0");
    cb_in_front_ = std::make_unique<DMCheckbox>("In Front (unchecked = Behind)", true);

    tool_panel_ = std::make_unique<FrameToolPanel>("Anchor Tool Panel", "frame_editor_tool_panel_anchor");
    back_widget_ = std::make_unique<ButtonWidget>(btn_back_.get(), [this]() {
        if (apply_form_to_anchor()) {
            dirty_ = true;
        }
        persist_pending_changes();
        wants_close_ = true;
    });
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
    save_widget_ = std::make_unique<ButtonWidget>(btn_save_.get(), [this]() {
        apply_form_to_anchor();
        persist_changes();
        if (context_.document) {
            context_.document->save_to_file_checked(true);
        }
        dirty_ = false;
    });
    anchor_list_widget_.reset(new AnchorListWidget(&frames_, &selected_frame_, &selected_anchor_, [this](int idx) {
        select_anchor(idx);
    }));
    anchor_list_widget_->set_selected_anchor_ref(&selected_anchor_);
    name_widget_ = std::make_unique<TextBoxWidget>(tb_name_.get(), true);
    tex_x_widget_ = std::make_unique<TextBoxWidget>(tb_tex_x_.get(), true);
    tex_y_widget_ = std::make_unique<TextBoxWidget>(tb_tex_y_.get(), true);
    in_front_widget_ = std::make_unique<CheckboxWidget>(cb_in_front_.get());
    rebuild_tool_panel_layout();
    // Position set on first update when screen dimensions are available.

    lock_target_to_selected_frame();
    hydrate_anchor_pixels_from_target();
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
    persist_pending_changes();
    restore_target_frame_lock();
    frames_.clear();
    tool_panel_.reset();
    back_widget_.reset();
    add_widget_.reset();
    delete_widget_.reset();
    save_widget_.reset();
    anchor_list_widget_.reset();
    name_widget_.reset();
    tex_x_widget_.reset();
    tex_y_widget_.reset();
    in_front_widget_.reset();
    frame_navigator_.reset();
    btn_back_.reset();
    btn_add_.reset();
    btn_delete_.reset();
    btn_save_.reset();
    if (selection_state_) {
        selection_state_->reset();
        selection_state_ = nullptr;
    }
    tb_name_.reset();
    tb_tex_x_.reset();
    tb_tex_y_.reset();
    cb_in_front_.reset();
    dirty_ = false;
    wants_close_ = false;
    target_frame_lock_active_ = false;
    saved_target_animation_.clear();
    saved_target_frame_ = nullptr;
    saved_target_static_frame_ = false;
}

bool AnchorFrameEditor::handle_event(const SDL_Event& e) {
    bool handled = false;

    if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE) {
        if (apply_form_to_anchor()) {
            dirty_ = true;
        }
        persist_pending_changes();
        wants_close_ = true;
        return true;
    }

    if (tool_panel_) {
        const std::string name_before = tb_name_ ? tb_name_->value() : std::string{};
        const std::string tx_before = tb_tex_x_ ? tb_tex_x_->value() : std::string{};
        const std::string tz_before = tb_tex_y_ ? tb_tex_y_->value() : std::string{};
        const bool front_before = cb_in_front_ ? cb_in_front_->value() : true;

        if (tool_panel_->handle_event(e)) {
            handled = true;
        }

        const bool text_changed =
            (tb_name_ && tb_name_->value() != name_before) ||
            (tb_tex_x_ && tb_tex_x_->value() != tx_before) ||
            (tb_tex_y_ && tb_tex_y_->value() != tz_before) ||
            (cb_in_front_ && cb_in_front_->value() != front_before);

        if (text_changed && apply_form_to_anchor()) {
            refresh_selection_state();
            dirty_ = true;
            handled = true;
        }
    }

    if (frame_navigator_ && frame_navigator_->handle_event(e)) {
        handled = true;
    }

    if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT) {
        SDL_Point mouse_pos{static_cast<int>(std::lround(e.button.x)), static_cast<int>(std::lround(e.button.y))};
        if (!ui_contains_point(mouse_pos) && selected_frame_ >= 0 && selected_frame_ < static_cast<int>(frames_.size())) {
            const auto& frame = frames_.at(static_cast<std::size_t>(selected_frame_));
            int closest = -1;
            float closest_dist_sq = 196.0f; // 14px radius
            for (std::size_t i = 0; i < frame.anchors.size(); ++i) {
                SDL_FPoint screen{};
                float world_z = 0.0f;
                if (!resolve_anchor_screen(static_cast<int>(i), screen, world_z)) {
                    continue;
                }
                const float dx = screen.x - static_cast<float>(mouse_pos.x);
                const float dy = screen.y - static_cast<float>(mouse_pos.y);
                const float dist_sq = dx * dx + dy * dy;
                if (dist_sq <= closest_dist_sq) {
                    closest_dist_sq = dist_sq;
                    closest = static_cast<int>(i);
                }
            }
            if (closest >= 0) {
                select_anchor(closest);
                // Start drag
                const auto& anchor = frame.anchors[static_cast<std::size_t>(closest)];
                is_dragging_ = true;
                drag_anchor_start_tex_x_ = anchor.texture_x;
                drag_anchor_start_tex_y_ = anchor.texture_y;
                handled = true;
            }
        }
    }

    if (e.type == SDL_EVENT_MOUSE_MOTION && is_dragging_ && selected_anchor_ >= 0) {
        SDL_Point mouse_pos{static_cast<int>(std::lround(e.motion.x)), static_cast<int>(std::lround(e.motion.y))};
        update_anchor_from_drag(mouse_pos);
        handled = true;
    }

    if (e.type == SDL_EVENT_MOUSE_BUTTON_UP && e.button.button == SDL_BUTTON_LEFT && is_dragging_) {
        is_dragging_ = false;
        handled = true;
    }

    return handled;
}

void AnchorFrameEditor::update(const Input& input, float) {
    lock_target_to_selected_frame();
    nav_rect_.x = 0;
    nav_rect_.y = 0;
    nav_rect_.w = screen_w_;
    nav_rect_.h = frame_navigator_ ? frame_navigator_->get_preferred_rect().h : 0;
    if (frame_navigator_) {
        frame_navigator_->set_rect(nav_rect_);
    }
    if (tool_panel_) {
        tool_panel_->set_work_area(SDL_Rect{0, 0, screen_w_, screen_h_});
        tool_panel_->set_position_if_unset(screen_w_, nav_rect_.h + DMSpacing::header_gap());
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
        SDL_Color color = is_selected ? kListAccent : SDL_Color{200, 200, 200, 255};
        SDL_Rect dot{
            static_cast<int>(std::lround(screen.x)) - 4,
            static_cast<int>(std::lround(screen.y)) - 4,
            8,
            8};
        dm_draw::DrawRoundedSolidRect(renderer, dot, 4, color);
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
        tool_panel_->set_position_if_unset(sw, nav_height + DMSpacing::header_gap());
    }
}

void AnchorFrameEditor::select_frame(int index) {
    // Apply pending textbox edits before switching frames.
    if (apply_form_to_anchor()) {
        dirty_ = true;
    }
    apply_live_changes(true);
    selected_frame_ = std::clamp(index, 0, static_cast<int>(frames_.size()) - 1);
    const auto& frame = frames_[static_cast<std::size_t>(selected_frame_)];
    if (frame.anchors.empty()) {
        selected_anchor_ = -1;
    } else if (selected_anchor_ < 0) {
        selected_anchor_ = 0;
    } else {
        selected_anchor_ = std::clamp(selected_anchor_, 0, static_cast<int>(frame.anchors.size()) - 1);
    }
    lock_target_to_selected_frame();
    refresh_form();
    refresh_selection_state();
    rebuild_tool_panel_layout();
    if (frame_navigator_) {
        frame_navigator_->set_current_frame(selected_frame_);
    }
}

void AnchorFrameEditor::select_anchor(int index) {
    // Apply pending textbox edits to the outgoing anchor before switching.
    if (apply_form_to_anchor()) {
        dirty_ = true;
    }

    const auto& frame = frames_.at(static_cast<std::size_t>(selected_frame_));
    if (frame.anchors.empty()) {
        selected_anchor_ = -1;
    } else {
        selected_anchor_ = std::clamp(index, 0, static_cast<int>(frame.anchors.size()) - 1);
    }
    refresh_form();
    refresh_selection_state();
    rebuild_tool_panel_layout();

    // Auto-persist so anchor edits survive any subsequent action.
    if (dirty_) {
        persist_pending_changes();
    }
}

void AnchorFrameEditor::refresh_form() {
    hydrate_anchor_pixels_from_target();
    if (!tb_name_) return;
    const auto& frame = frames_.at(static_cast<std::size_t>(selected_frame_));
    if (selected_anchor_ >= 0 && selected_anchor_ < static_cast<int>(frame.anchors.size())) {
        const auto& a = frame.anchors[static_cast<std::size_t>(selected_anchor_)];
        tb_name_->set_value(a.name);
        tb_tex_x_->set_value(std::to_string(a.texture_x));
        tb_tex_y_->set_value(std::to_string(a.texture_y));
        if (cb_in_front_) cb_in_front_->set_value(a.in_front);
    } else {
        tb_name_->set_value("");
        tb_tex_x_->set_value("0");
        tb_tex_y_->set_value("0");
        if (cb_in_front_) cb_in_front_->set_value(true);
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

    const int new_tex_x = std::max(0, parse_int_box(tb_tex_x_.get(), a.texture_x));
    const int new_tex_y = std::max(0, parse_int_box(tb_tex_y_.get(), a.texture_y));
    const bool new_front = cb_in_front_ ? cb_in_front_->value() : a.in_front;

    if (a.texture_x != new_tex_x) {
        a.texture_x = new_tex_x;
        changed = true;
    }
    if (a.texture_y != new_tex_y) {
        a.texture_y = new_tex_y;
        changed = true;
    }
    if (a.in_front != new_front) {
        a.in_front = new_front;
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
    hydrate_anchor_pixels_from_target();
    apply_to_all_frames();
    persist_changes();
    persist_pending_changes();
}

bool AnchorFrameEditor::apply_to_all_animations() {
    if (!context_.document || frames_.empty() || selected_frame_ < 0 || selected_frame_ >= static_cast<int>(frames_.size())) return false;
    hydrate_anchor_pixels_from_target();
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
        sync_runtime_animation_anchors(id, frames);
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
    hydrate_anchor_pixels_from_target();
    auto payload_opt = context_.document->animation_payload_json(context_.animation_id);
    nlohmann::json existing = payload_opt.value_or(nlohmann::json::object());
    nlohmann::json updated = build_payload_with_anchors(frames_, existing);
    context_.document->update_animation_payload(context_.animation_id, updated);
    sync_runtime_animation_anchors(context_.animation_id, frames_);
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
        selection_state_->target = SelectionTarget::None;
        selection_state_->clear_anchor_world();
        return;
    }

    selection_state_->target = SelectionTarget::AnchorPoint;
    selection_state_->world_pos = world;
    selection_state_->world_z = world_z;
    selection_state_->screen_pos = round_point(screen);
    selection_state_->set_anchor_world(context_.target->world_point(), static_cast<float>(context_.target->world_z()));
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
    // Allow read-only code paths to populate derived pixel coords.
    const_cast<AnchorFrameEditor*>(this)->hydrate_anchor_pixels_from_target();
    const auto& frame = frames_.at(static_cast<std::size_t>(selected_frame_));
    if (anchor_index >= static_cast<int>(frame.anchors.size())) {
        return false;
    }
    const auto& anchor = frame.anchors[static_cast<std::size_t>(anchor_index)];
    const auto runtime_anchor = to_runtime_anchor(anchor);
    const auto pixel_locked = anchor_points::resolve_pixel_locked_anchor(
        *context_.target,
        runtime_anchor,
        anchor_points::GridMaterialization::None);

    if (pixel_locked.resolved.missing) {
        return false;
    }

    out_world_z = static_cast<float>(pixel_locked.resolved.world_z);
    if (out_world) {
        *out_world = SDL_FPoint{static_cast<float>(pixel_locked.resolved.world_px.x),
                                static_cast<float>(pixel_locked.resolved.world_px.y)};
    }
    out_screen = pixel_locked.screen_px;
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

void AnchorFrameEditor::update_anchor_from_drag(SDL_Point mouse_screen) {
    if (selected_anchor_ < 0 || selected_frame_ < 0 ||
        selected_frame_ >= static_cast<int>(frames_.size())) {
        return;
    }
    auto& frame = frames_.at(static_cast<std::size_t>(selected_frame_));
    if (selected_anchor_ >= static_cast<int>(frame.anchors.size())) {
        return;
    }
    if (!context_.target || !context_.assets) {
        return;
    }

    hydrate_anchor_pixels_from_target();
    auto& anchor = frame.anchors[static_cast<std::size_t>(selected_anchor_)];

    // Resolve current screen position of the anchor.
    const auto base_result = anchor_points::resolve_pixel_locked_anchor(
        *context_.target,
        to_runtime_anchor(anchor),
        anchor_points::GridMaterialization::None);
    if (base_result.resolved.missing) {
        return;
    }

    const SDL_FPoint base_screen = base_result.screen_px;
    const float screen_err_x = static_cast<float>(mouse_screen.x) - base_screen.x;
    const float screen_err_y = static_cast<float>(mouse_screen.y) - base_screen.y;

    // If already close enough, skip update.
    if (screen_err_x * screen_err_x + screen_err_y * screen_err_y < 0.25f) {
        return;
    }

    // Build numerical Jacobian using perturbation delta.
    constexpr int kDelta = 5;

    DisplacedAssetAnchorPoint dx_anchor = to_runtime_anchor(anchor);
    dx_anchor.texture_x += kDelta;
    DisplacedAssetAnchorPoint dz_anchor = to_runtime_anchor(anchor);
    dz_anchor.texture_y += kDelta;

    const auto dx_result = anchor_points::resolve_pixel_locked_anchor(
        *context_.target,
        dx_anchor,
        anchor_points::GridMaterialization::None);
    const auto dz_result = anchor_points::resolve_pixel_locked_anchor(
        *context_.target,
        dz_anchor,
        anchor_points::GridMaterialization::None);

    if (dx_result.resolved.missing || dz_result.resolved.missing) {
        return;
    }

    // Jacobian columns: d(screen) / d(tex_x) and d(screen) / d(tex_y)
    const float inv_delta = 1.0f / static_cast<float>(kDelta);
    const float j00 = (dx_result.screen_px.x - base_screen.x) * inv_delta; // dSx/dTx
    const float j10 = (dx_result.screen_px.y - base_screen.y) * inv_delta; // dSy/dTx
    const float j01 = (dz_result.screen_px.x - base_screen.x) * inv_delta; // dSx/dTz
    const float j11 = (dz_result.screen_px.y - base_screen.y) * inv_delta; // dSy/dTz

    // Invert 2x2 Jacobian.
    const float det = j00 * j11 - j01 * j10;
    if (std::abs(det) < 1e-6f) {
        return;
    }
    const float inv_det = 1.0f / det;
    const float dtex_x = inv_det * (j11 * screen_err_x - j01 * screen_err_y);
    const float dtex_y = inv_det * (-j10 * screen_err_x + j00 * screen_err_y);

    const int new_tex_x = std::max(0, anchor.texture_x + static_cast<int>(std::lround(dtex_x)));
    const int new_tex_y = std::max(0, anchor.texture_y + static_cast<int>(std::lround(dtex_y)));

    if (new_tex_x != anchor.texture_x || new_tex_y != anchor.texture_y) {
        anchor.texture_x = new_tex_x;
        anchor.texture_y = new_tex_y;
        refresh_form();
        refresh_selection_state();
        dirty_ = true;
    }
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
        if (save_widget_) rows.push_back({save_widget_.get()});
        if (tex_x_widget_) rows.push_back({tex_x_widget_.get()});
        if (tex_y_widget_) rows.push_back({tex_y_widget_.get()});
        if (in_front_widget_) rows.push_back({in_front_widget_.get()});
    }

    tool_panel_->set_rows(rows);
}

std::pair<int, int> AnchorFrameEditor::frame_dimensions_for_index(std::size_t frame_index) const {
    int frame_w = 0;
    int frame_h = 0;
    if (!context_.target || !context_.target->info) {
        return {frame_w, frame_h};
    }

    const auto anim_it = context_.target->info->animations.find(context_.animation_id);
    if (anim_it != context_.target->info->animations.end()) {
        const Animation& anim = anim_it->second;
        if (anim.movement_path_count() > 0) {
            const auto& path = anim.movement_path(0);
            if (frame_index < path.size()) {
                const AnimationFrame& frame = path[frame_index];
                if (!frame.variants.empty()) {
                    const int variant_idx = std::clamp(
                        context_.target->current_variant_index,
                        0,
                        static_cast<int>(frame.variants.size()) - 1);
                    const FrameVariant& variant = frame.variants[static_cast<std::size_t>(variant_idx)];
                    if (variant.source_rect.w > 0 && variant.source_rect.h > 0) {
                        frame_w = variant.source_rect.w;
                        frame_h = variant.source_rect.h;
                    }
                }
            }
        }
    }

    if (frame_w <= 0 || frame_h <= 0) {
        frame_w = context_.target->info->original_canvas_width;
        frame_h = context_.target->info->original_canvas_height;
    }

    return {frame_w, frame_h};
}

std::pair<int, int> AnchorFrameEditor::current_frame_dimensions() const {
    if (selected_frame_ < 0) {
        return frame_dimensions_for_index(0);
    }
    return frame_dimensions_for_index(static_cast<std::size_t>(selected_frame_));
}

void AnchorFrameEditor::hydrate_anchor_pixels_from_target() {
}

void AnchorFrameEditor::lock_target_to_selected_frame() {
    if (!context_.target || !context_.target->info) {
        return;
    }
    auto anim_it = context_.target->info->animations.find(context_.animation_id);
    if (anim_it == context_.target->info->animations.end() || anim_it->second.frames.empty()) {
        return;
    }
    const int max_index = static_cast<int>(anim_it->second.frames.size()) - 1;
    const int index = std::clamp(selected_frame_, 0, max_index);
    AnimationFrame* frame = anim_it->second.frames[static_cast<std::size_t>(index)];
    if (!frame) {
        return;
    }

    bool changed = false;
    if (context_.target->current_animation != context_.animation_id) {
        context_.target->current_animation = context_.animation_id;
        changed = true;
    }
    if (context_.target->current_frame != frame) {
        context_.target->current_frame = frame;
        changed = true;
    }
    if (!context_.target->static_frame) {
        context_.target->static_frame = true;
        changed = true;
    }
    if (changed) {
        context_.target->mark_anchors_dirty();
    }
}

void AnchorFrameEditor::restore_target_frame_lock() {
    if (!target_frame_lock_active_ || !context_.target) {
        return;
    }
    bool changed = false;
    if (context_.target->current_animation != saved_target_animation_) {
        context_.target->current_animation = saved_target_animation_;
        changed = true;
    }
    if (context_.target->current_frame != saved_target_frame_) {
        context_.target->current_frame = saved_target_frame_;
        changed = true;
    }
    if (context_.target->static_frame != saved_target_static_frame_) {
        context_.target->static_frame = saved_target_static_frame_;
        changed = true;
    }
    if (changed) {
        context_.target->mark_anchors_dirty();
    }
}

void AnchorFrameEditor::sync_runtime_animation_anchors(const std::string& animation_id,
                                                       const std::vector<AnchorFrame>& frames) {
    if (!context_.target || !context_.target->info || animation_id.empty()) {
        return;
    }
    auto anim_it = context_.target->info->animations.find(animation_id);
    if (anim_it == context_.target->info->animations.end()) {
        return;
    }
    Animation& animation = anim_it->second;
    if (animation.frames.empty()) {
        return;
    }

    const std::size_t count = std::min(frames.size(), animation.frames.size());
    for (std::size_t i = 0; i < count; ++i) {
        AnimationFrame* runtime_frame = animation.frames[i];
        if (!runtime_frame) {
            continue;
        }
        std::vector<DisplacedAssetAnchorPoint> runtime_anchors;
        runtime_anchors.reserve(frames[i].anchors.size());
        std::unordered_set<std::string> seen_names;
        for (const auto& anchor : frames[i].anchors) {
            if (anchor.name.empty()) {
                continue;
            }
            if (!seen_names.insert(anchor.name).second) {
                continue;
            }
            DisplacedAssetAnchorPoint runtime_anchor = to_runtime_anchor(anchor);
            runtime_anchors.push_back(std::move(runtime_anchor));
        }
        runtime_frame->set_anchor_points(std::move(runtime_anchors));
    }

    context_.target->mark_anchors_dirty();
}

DisplacedAssetAnchorPoint AnchorFrameEditor::to_runtime_anchor(const FrameAnchorPoint& anchor) const {
    DisplacedAssetAnchorPoint runtime(anchor.name, anchor.texture_x, anchor.texture_y, anchor.in_front);
    return runtime;
}

}  // namespace devmode::frame_editors
