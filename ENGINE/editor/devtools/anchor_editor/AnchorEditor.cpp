#include "AnchorEditor.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_set>

#include "assets/Asset.hpp"
#include "assets/animation_frame.hpp"
#include "assets/asset/animation.hpp"
#include "devtools/dm_styles.hpp"
#include "devtools/draw_utils.hpp"
#include "devtools/font_cache.hpp"
#include "devtools/frame_editors/shared/SelectionState.hpp"

namespace devmode::anchor_editor {

namespace {
constexpr SDL_Color kSelectedColor{90, 200, 255, 255};
int parse_int_box(DMTextBox* box, int fallback = 0) {
    if (!box) return fallback;
    try { return std::stoi(box->value()); } catch (...) { return fallback; }
}
}

class AnchorListWidget : public Widget {
public:
    AnchorListWidget(const std::vector<frame_editors::AnchorFrame>* frames, const int* selected_frame,
                     const int* selected_anchor, std::function<void(int)> on_select)
        : frames_(frames), selected_frame_(selected_frame), selected_anchor_(selected_anchor), on_select_(std::move(on_select)) {}
    void set_rect(const SDL_Rect& r) override { rect_ = r; }
    const SDL_Rect& rect() const override { return rect_; }
    int height_for_width(int) const override { return std::max(40, count() * (DMButton::height() + 4) + 8); }
    bool wants_full_row() const override { return true; }
    bool handle_event(const SDL_Event& e) override {
        if (e.type != SDL_EVENT_MOUSE_BUTTON_DOWN || e.button.button != SDL_BUTTON_LEFT) return false;
        SDL_Point p{static_cast<int>(std::lround(e.button.x)), static_cast<int>(std::lround(e.button.y))};
        if (!SDL_PointInRect(&p, &rect_)) return false;
        int y = rect_.y + 4;
        for (int i = 0; i < count(); ++i) {
            SDL_Rect row{rect_.x + 4, y, std::max(0, rect_.w - 8), DMButton::height()};
            if (SDL_PointInRect(&p, &row)) { if (on_select_) on_select_(i); break; }
            y += DMButton::height() + 4;
        }
        return true;
    }
    void render(SDL_Renderer* renderer) const override {
        if (!renderer) return;
        dm_draw::DrawRoundedSolidRect(renderer, rect_, 8, DMStyles::PanelBG());
        const auto* anchors = list();
        if (!anchors) return;
        int y = rect_.y + 4;
        for (int i = 0; i < static_cast<int>(anchors->size()); ++i) {
            SDL_Rect row{rect_.x + 4, y, std::max(0, rect_.w - 8), DMButton::height()};
            const bool sel = selected_anchor_ && *selected_anchor_ == i;
            dm_draw::DrawRoundedSolidRect(renderer, row, 6, sel ? DMStyles::AccentButton().bg : DMStyles::ButtonBaseFill());
            DMFontCache::instance().draw_text(renderer, DMStyles::Label(), (*anchors)[i].name, row.x + 6, row.y + 6);
            y += DMButton::height() + 4;
        }
    }
private:
    const std::vector<frame_editors::FrameAnchorPoint>* list() const {
        if (!frames_ || !selected_frame_ || *selected_frame_ < 0 || *selected_frame_ >= static_cast<int>(frames_->size())) return nullptr;
        return &frames_->at(static_cast<std::size_t>(*selected_frame_)).anchors;
    }
    int count() const { auto* l = list(); return l ? static_cast<int>(l->size()) : 0; }
    SDL_Rect rect_{0,0,0,0};
    const std::vector<frame_editors::AnchorFrame>* frames_ = nullptr;
    const int* selected_frame_ = nullptr;
    const int* selected_anchor_ = nullptr;
    std::function<void(int)> on_select_;
};

void AnchorListWidgetDeleter::operator()(AnchorListWidget* ptr) const noexcept { delete ptr; }

void AnchorEditor::begin(const frame_editors::FrameEditorContext& context) {
    context_ = context;
    selection_state_ = context.selection_state;
    texture_primed_ = false;
    tried_runtime_rebuild_ = false;
    preview_refreshed_ = false;
    if (context.document) {
        auto payload = context.document->animation_payload_json(context.animation_id);
        frames_ = frame_editors::parse_anchor_frames_from_payload(payload.value_or(nlohmann::json::object()));
    }
    int preview_frames = 0;
    if (context.preview) {
        preview_frames = context.preview->get_frame_count(context.animation_id);
        if (preview_frames > 0 && static_cast<std::size_t>(preview_frames) > frames_.size()) {
            frames_.resize(static_cast<std::size_t>(preview_frames));
        }
    }
    int runtime_frames = 0;
    if (context_.target && context_.target->info) {
        auto it = context_.target->info->animations.find(context_.animation_id);
        if (it != context_.target->info->animations.end()) {
            runtime_frames = static_cast<int>(it->second.frames.size());
            if (runtime_frames > 0 && static_cast<std::size_t>(runtime_frames) > frames_.size()) {
                frames_.resize(static_cast<std::size_t>(runtime_frames));
            }
        }
    }
    // Force fresh preview textures in case a stale cache is empty.
    prime_textures();
    if (frames_.empty()) frames_.emplace_back();

    frame_navigator_ = std::make_unique<frame_editors::FrameNavigator>();
    frame_navigator_->set_frame_count(static_cast<int>(frames_.size()));
    frame_navigator_->set_current_frame(0);
    frame_navigator_->set_on_frame_changed([this](int i){ select_frame(i); });
    frame_navigator_->set_on_apply_next([this](){ apply_to_next_frame(); });
    frame_navigator_->set_on_apply_animation([this](){ (void)apply_to_selected_animations(); });
    frame_navigator_->set_on_apply_all([this](){ (void)apply_to_all_animations(); });
    frame_navigator_->set_preview_source(context.preview, context.animation_id);

    btn_back_ = std::make_unique<DMButton>("Exit & Save", &DMStyles::DeleteButton(), 140, DMButton::height());
    btn_add_ = std::make_unique<DMButton>("Add Anchor", &DMStyles::AccentButton(), 140, DMButton::height());
    btn_delete_ = std::make_unique<DMButton>("Delete Anchor", &DMStyles::DeleteButton(), 140, DMButton::height());
    btn_save_ = std::make_unique<DMButton>("Save", &DMStyles::AccentButton(), 140, DMButton::height());
    tb_name_ = std::make_unique<DMTextBox>("Name", "");
    tb_tex_x_ = std::make_unique<DMTextBox>("Texture X (px)", "0");
    tb_tex_y_ = std::make_unique<DMTextBox>("Texture Y (px)", "0");
    cb_in_front_ = std::make_unique<DMCheckbox>("In Front", true);

    tool_panel_ = std::make_unique<frame_editors::FrameToolPanel>("Anchor Editor", "anchor_editor_tool_panel");
    back_widget_ = std::make_unique<ButtonWidget>(btn_back_.get(), [this](){ request_close(); });
    add_widget_ = std::make_unique<ButtonWidget>(btn_add_.get(), [this](){ add_anchor(); });
    delete_widget_ = std::make_unique<ButtonWidget>(btn_delete_.get(), [this](){
        auto& list = frames_[static_cast<std::size_t>(selected_frame_)].anchors;
        if (selected_anchor_ >= 0 && selected_anchor_ < static_cast<int>(list.size())) {
            list.erase(list.begin() + selected_anchor_);
            selected_anchor_ = std::clamp(selected_anchor_, 0, static_cast<int>(list.size()) - 1);
            dirty_ = true;
            refresh_form();
        }
    });
    save_widget_ = std::make_unique<ButtonWidget>(btn_save_.get(), [this](){ persist_changes(); persist_pending_changes(); });
    anchor_list_widget_.reset(new AnchorListWidget(&frames_, &selected_frame_, &selected_anchor_, [this](int i){ select_anchor(i); }));
    name_widget_ = std::make_unique<TextBoxWidget>(tb_name_.get(), true);
    tex_x_widget_ = std::make_unique<TextBoxWidget>(tb_tex_x_.get(), true);
    tex_y_widget_ = std::make_unique<TextBoxWidget>(tb_tex_y_.get(), true);
    in_front_widget_ = std::make_unique<CheckboxWidget>(cb_in_front_.get());
    rebuild_tool_panel_layout();

    hydrate_anchor_pixels_from_target();
    if (frame_navigator_) {
        frame_navigator_->set_frame_count(static_cast<int>(frames_.size()));
    }
    if (!frames_[0].anchors.empty()) selected_anchor_ = 0;
    refresh_form();
    center_view();

    manifest_txn_.begin(context_);
    manifest_txn_.set_immediate_persist(false);
    manifest_txn_.set_apply_callback([this]() { persist_changes(); return true; });
}

void AnchorEditor::end() { persist_pending_changes(); }

bool AnchorEditor::handle_event(const SDL_Event& e) {
    if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE) { request_close(); return true; }
    bool handled = false;
    if (tool_panel_ && tool_panel_->handle_event(e)) { if (apply_form_to_anchor()) dirty_ = true; handled = true; }
    if (frame_navigator_ && frame_navigator_->handle_event(e)) handled = true;

    if (e.type == SDL_EVENT_MOUSE_WHEEL) {
        SDL_Point p{static_cast<int>(e.wheel.mouse_x), static_cast<int>(e.wheel.mouse_y)};
        if (point_in_viewport(p)) {
            int tx = 0, ty = 0;
            screen_to_texture(p, tx, ty);
            SDL_FPoint before = texture_to_screen(tx, ty);
            const float delta = (e.wheel.y > 0) ? 1.1f : 0.9f;
            zoom_ = std::clamp(zoom_ * delta, 0.25f, 64.0f);
            SDL_FPoint after = texture_to_screen(tx, ty);
            pan_.x += before.x - after.x;
            pan_.y += before.y - after.y;
            return true;
        }
    }

    if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
        SDL_Point p{static_cast<int>(std::lround(e.button.x)), static_cast<int>(std::lround(e.button.y))};
        if (e.button.button == SDL_BUTTON_MIDDLE && point_in_viewport(p)) { is_panning_ = true; pan_last_ = p; return true; }
        if (e.button.button == SDL_BUTTON_LEFT && point_in_viewport(p) && !ui_contains_point(p)) {
            int tx = 0, ty = 0;
            if (screen_to_texture(p, tx, ty)) {
                auto& frame = frames_[static_cast<std::size_t>(selected_frame_)];
                int closest = -1; float best = 100.0f;
                for (int i = 0; i < static_cast<int>(frame.anchors.size()); ++i) {
                    SDL_FPoint s = texture_to_screen(frame.anchors[i].texture_x, frame.anchors[i].texture_y);
                    const float dx = s.x - p.x, dy = s.y - p.y, d = dx*dx + dy*dy;
                    if (d < best) { best = d; closest = i; }
                }
                if (closest >= 0) select_anchor(closest);
                if (selected_anchor_ >= 0 && selected_anchor_ < static_cast<int>(frame.anchors.size())) {
                    auto& a = frame.anchors[static_cast<std::size_t>(selected_anchor_)];
                    a.texture_x = tx; a.texture_y = ty;
                    dirty_ = true; refresh_form(); refresh_selection_state();
                    is_dragging_anchor_ = true;
                }
                return true;
            }
        }
    }
    if (e.type == SDL_EVENT_MOUSE_MOTION) {
        SDL_Point p{static_cast<int>(std::lround(e.motion.x)), static_cast<int>(std::lround(e.motion.y))};
        if (is_panning_) { pan_.x += static_cast<float>(p.x - pan_last_.x); pan_.y += static_cast<float>(p.y - pan_last_.y); pan_last_ = p; return true; }
        if (is_dragging_anchor_) { update_drag_pick(p); return true; }
    }
    if (e.type == SDL_EVENT_MOUSE_BUTTON_UP) {
        if (e.button.button == SDL_BUTTON_MIDDLE) is_panning_ = false;
        if (e.button.button == SDL_BUTTON_LEFT) is_dragging_anchor_ = false;
    }
    return handled;
}

void AnchorEditor::update(const Input& input, float) {
    layout_ui();
    if (tool_panel_) { tool_panel_->set_work_area(SDL_Rect{0,0,screen_w_,screen_h_}); tool_panel_->update(input, screen_w_, screen_h_); }
    if (!texture_primed_) prime_textures();
    refresh_selection_state();
}

void AnchorEditor::render_world(SDL_Renderer*) const {}

void AnchorEditor::render_overlays(SDL_Renderer* renderer) const {
    if (!renderer) return;
    if (!texture_primed_) const_cast<AnchorEditor*>(this)->prime_textures();
    layout_ui(renderer);
    SDL_SetRenderDrawColor(renderer, 18, 20, 24, 255);
    SDL_FRect vp{static_cast<float>(viewport_rect_.x), static_cast<float>(viewport_rect_.y), static_cast<float>(viewport_rect_.w), static_cast<float>(viewport_rect_.h)};
    SDL_RenderFillRect(renderer, &vp);
    const auto tex_info = resolve_frame_texture(renderer, selected_frame_);
    if (tex_info.texture) {
        float tw = tex_info.has_src_rect ? static_cast<float>(tex_info.src_rect.w) : 0.0f;
        float th = tex_info.has_src_rect ? static_cast<float>(tex_info.src_rect.h) : 0.0f;
        if (tw <= 0.0f || th <= 0.0f) {
            SDL_GetTextureSize(tex_info.texture, &tw, &th);
        }
        if (tw > 0.0f && th > 0.0f) {
            SDL_FRect dst{viewport_rect_.x + pan_.x + (viewport_rect_.w - tw * zoom_) * 0.5f,
                          viewport_rect_.y + pan_.y + (viewport_rect_.h - th * zoom_) * 0.5f,
                          tw * zoom_, th * zoom_};
            SDL_FRect src_f{};
            const SDL_FRect* src_ptr = nullptr;
            if (tex_info.has_src_rect) {
                src_f = SDL_FRect{static_cast<float>(tex_info.src_rect.x),
                                  static_cast<float>(tex_info.src_rect.y),
                                  static_cast<float>(tex_info.src_rect.w),
                                  static_cast<float>(tex_info.src_rect.h)};
                src_ptr = &src_f;
            }
            SDL_SetTextureScaleMode(tex_info.texture, SDL_SCALEMODE_NEAREST);
            SDL_RenderTexture(renderer, tex_info.texture, src_ptr, &dst);
            texture_primed_ = true;
        } else if (!tried_runtime_rebuild_) {
            tried_runtime_rebuild_ = true;
            if (context_.target) context_.target->rebuild_animation_runtime();
            prime_textures();
        }
    }
    const auto& frame = frames_.at(static_cast<std::size_t>(selected_frame_));
    for (int i = 0; i < static_cast<int>(frame.anchors.size()); ++i) {
        SDL_FPoint p = texture_to_screen(frame.anchors[i].texture_x, frame.anchors[i].texture_y);
        SDL_Rect dot{static_cast<int>(p.x)-4, static_cast<int>(p.y)-4, 8, 8};
        dm_draw::DrawRoundedSolidRect(renderer, dot, 4, i == selected_anchor_ ? kSelectedColor : SDL_Color{220,220,220,255});
    }
    DMFontCache::instance().draw_text(renderer, DMStyles::Label(), "Zoom: " + std::to_string(static_cast<int>(zoom_ * 100.0f)) + "% (wheel), Pan: middle-drag", viewport_rect_.x + 8, viewport_rect_.y + 8);
    if (frame_navigator_) frame_navigator_->render(renderer);
    if (tool_panel_) tool_panel_->render(renderer);
}

void AnchorEditor::persist_pending_changes() {
    if (manifest_txn_.active() && dirty_ && manifest_txn_.commit(true)) dirty_ = false;
}

void AnchorEditor::request_close() {
    if (apply_form_to_anchor()) dirty_ = true;
    // Ensure last edits are written before closing.
    persist_changes();
    if (manifest_txn_.active()) {
        manifest_txn_.commit(true);
        dirty_ = false;
    }
    wants_close_ = true;
}

void AnchorEditor::prime_textures() {
    SDL_Renderer* renderer = context_.assets ? context_.assets->renderer() : nullptr;
    if (context_.preview && renderer && !preview_refreshed_) {
        preview_refreshed_ = true;
        context_.preview->invalidate(context_.animation_id);
        context_.preview->get_frame_texture(renderer, context_.animation_id, 0);
    }
    if (context_.target && !tried_runtime_rebuild_) {
        tried_runtime_rebuild_ = true;
        context_.target->rebuild_animation_runtime();
    }
}

void AnchorEditor::layout_ui(SDL_Renderer* renderer) const {
    int sw = screen_w_, sh = screen_h_;
    if (renderer) SDL_GetCurrentRenderOutputSize(renderer, &sw, &sh);
    screen_w_ = sw; screen_h_ = sh;
    const int nav_h = frame_navigator_ ? frame_navigator_->get_preferred_rect().h : 0;
    nav_rect_ = SDL_Rect{0,0,sw,nav_h};
    if (frame_navigator_) frame_navigator_->set_rect(nav_rect_);
    viewport_rect_ = SDL_Rect{0, nav_h, sw, sh - nav_h};
    if (tool_panel_) { tool_panel_->set_work_area(SDL_Rect{0,0,sw,sh}); tool_panel_->set_position_if_unset(sw, nav_h + 8); }
}

void AnchorEditor::rebuild_tool_panel_layout() {
    DockableCollapsible::Rows rows;
    rows.push_back({back_widget_.get()});
    rows.push_back({anchor_list_widget_.get()});
    rows.push_back({add_widget_.get(), delete_widget_.get()});
    if (selected_anchor_ >= 0) rows.push_back({name_widget_.get()}), rows.push_back({save_widget_.get()}), rows.push_back({tex_x_widget_.get()}), rows.push_back({tex_y_widget_.get()}), rows.push_back({in_front_widget_.get()});
    if (tool_panel_) tool_panel_->set_rows(rows);
}

void AnchorEditor::refresh_form() {
    if (!tb_name_) return;
    auto& list = frames_[static_cast<std::size_t>(selected_frame_)].anchors;
    if (selected_anchor_ >= 0 && selected_anchor_ < static_cast<int>(list.size())) {
        const auto& a = list[static_cast<std::size_t>(selected_anchor_)];
        tb_name_->set_value(a.name); tb_tex_x_->set_value(std::to_string(a.texture_x)); tb_tex_y_->set_value(std::to_string(a.texture_y)); cb_in_front_->set_value(a.in_front);
    } else { tb_name_->set_value(""); tb_tex_x_->set_value("0"); tb_tex_y_->set_value("0"); cb_in_front_->set_value(true); }
}

bool AnchorEditor::apply_form_to_anchor() {
    auto& list = frames_[static_cast<std::size_t>(selected_frame_)].anchors;
    if (selected_anchor_ < 0 || selected_anchor_ >= static_cast<int>(list.size())) return false;
    auto& a = list[static_cast<std::size_t>(selected_anchor_)];
    bool changed = false;
    if (tb_name_ && !tb_name_->value().empty() && tb_name_->value() != a.name) { a.name = tb_name_->value(); changed = true; }
    int tx = std::max(0, parse_int_box(tb_tex_x_.get(), a.texture_x));
    int ty = std::max(0, parse_int_box(tb_tex_y_.get(), a.texture_y));
    if (tx != a.texture_x || ty != a.texture_y) { a.texture_x = tx; a.texture_y = ty; changed = true; }
    if (cb_in_front_ && cb_in_front_->value() != a.in_front) { a.in_front = cb_in_front_->value(); changed = true; }

    return changed;
}

void AnchorEditor::select_frame(int index) { if (apply_form_to_anchor()) dirty_ = true; selected_frame_ = std::clamp(index, 0, static_cast<int>(frames_.size()) - 1); if (!frames_[selected_frame_].anchors.empty()) selected_anchor_ = std::clamp(selected_anchor_, 0, static_cast<int>(frames_[selected_frame_].anchors.size()) - 1); else selected_anchor_ = -1; refresh_form(); }
void AnchorEditor::select_anchor(int index) { selected_anchor_ = index; refresh_form(); rebuild_tool_panel_layout(); }
void AnchorEditor::add_anchor() { auto& list = frames_[selected_frame_].anchors; frame_editors::FrameAnchorPoint p; p.name = "anchor_" + std::to_string(list.size()); list.push_back(p); select_anchor(static_cast<int>(list.size()) - 1); dirty_ = true; }
void AnchorEditor::apply_to_all_frames() { for (auto& f : frames_) f = frames_[selected_frame_]; dirty_ = true; }
void AnchorEditor::apply_to_next_frame() { if (frames_.empty()) return; frames_[(selected_frame_ + 1) % static_cast<int>(frames_.size())] = frames_[selected_frame_]; dirty_ = true; persist_changes(); }
void AnchorEditor::apply_to_animation() { apply_to_all_frames(); persist_changes(); }

bool AnchorEditor::apply_to_selected_animations() {
    if (!context_.document) return false;
    std::vector<std::string> ids;
    if (context_.selected_animation_ids_provider) {
        ids = context_.selected_animation_ids_provider();
    }
    if (ids.empty() && !context_.animation_id.empty()) {
        ids.push_back(context_.animation_id);
    }
    if (ids.empty()) return false;

    auto source = frames_[selected_frame_];
    bool changed = false;
    for (const auto& id : ids) {
        auto payload = context_.document->animation_payload_json(id).value_or(nlohmann::json::object());
        auto frames = frame_editors::parse_anchor_frames_from_payload(payload);
        if (frames.empty()) frames.emplace_back();
        for (auto& f : frames) {
            frame_editors::apply_anchor_scope(f, source, frame_editors::AnchorConflictPolicy::SyncExact);
        }
        context_.document->update_animation_payload(id, frame_editors::build_payload_with_anchors(frames, payload));
        sync_runtime_animation_anchors(id, frames);
        changed = true;
    }
    if (changed) {
        context_.document->save_to_file_checked(true);
    }
    return changed;
}

bool AnchorEditor::apply_to_all_animations() { if (!context_.document) return false; auto source = frames_[selected_frame_]; for (const auto& id : context_.document->animation_ids()) { auto payload = context_.document->animation_payload_json(id).value_or(nlohmann::json::object()); auto frames = frame_editors::parse_anchor_frames_from_payload(payload); if (frames.empty()) frames.emplace_back(); for (auto& f : frames) frame_editors::apply_anchor_scope(f, source, frame_editors::AnchorConflictPolicy::SyncExact); context_.document->update_animation_payload(id, frame_editors::build_payload_with_anchors(frames, payload)); sync_runtime_animation_anchors(id, frames);} context_.document->save_to_file_checked(true); return true; }

void AnchorEditor::persist_changes() {
    if (!context_.document) return;
    apply_form_to_anchor();
    auto payload = context_.document->animation_payload_json(context_.animation_id).value_or(nlohmann::json::object());
    context_.document->update_animation_payload(context_.animation_id, frame_editors::build_payload_with_anchors(frames_, payload));
    sync_runtime_animation_anchors(context_.animation_id, frames_);
    invalidate_preview();
    prime_textures();
}

void AnchorEditor::invalidate_preview() const { if (context_.preview) context_.preview->invalidate(context_.animation_id); }

void AnchorEditor::refresh_selection_state() {
    if (!selection_state_) return;
    auto& list = frames_[selected_frame_].anchors;
    if (selected_anchor_ < 0 || selected_anchor_ >= static_cast<int>(list.size())) { selection_state_->target = frame_editors::SelectionTarget::None; return; }
    const auto& a = list[static_cast<std::size_t>(selected_anchor_)];
    SDL_FPoint s = texture_to_screen(a.texture_x, a.texture_y);
    selection_state_->target = frame_editors::SelectionTarget::AnchorPoint;
    selection_state_->screen_pos = SDL_Point{static_cast<int>(std::lround(s.x)), static_cast<int>(std::lround(s.y))};
}

bool AnchorEditor::ui_contains_point(const SDL_Point& p) const { return SDL_PointInRect(&p, &nav_rect_) || (tool_panel_ && tool_panel_->contains_point(p)); }
std::pair<int, int> AnchorEditor::frame_dimensions_for_index(std::size_t frame_index) const {
    SDL_Renderer* renderer = context_.assets ? context_.assets->renderer() : nullptr;
    FrameTextureInfo info = resolve_frame_texture(renderer, static_cast<int>(frame_index));
    if (!info.texture) return {0, 0};
    int w = info.has_src_rect ? info.src_rect.w : 0;
    int h = info.has_src_rect ? info.src_rect.h : 0;
    if (w <= 0 || h <= 0) {
        float tw = 0, th = 0;
        SDL_GetTextureSize(info.texture, &tw, &th);
        w = static_cast<int>(tw);
        h = static_cast<int>(th);
    }
    return {w, h};
}
std::pair<int, int> AnchorEditor::current_frame_dimensions() const { return frame_dimensions_for_index(static_cast<std::size_t>(selected_frame_)); }
void AnchorEditor::hydrate_anchor_pixels_from_target() {
    if (!context_.target || !context_.target->info) return;
    auto it = context_.target->info->animations.find(context_.animation_id);
    if (it == context_.target->info->animations.end()) return;
    const auto& anim = it->second;
    if (anim.frames.empty()) return;
    if (frames_.size() < anim.frames.size()) {
        frames_.resize(anim.frames.size());
    }
    for (std::size_t i = 0; i < anim.frames.size(); ++i) {
        const AnimationFrame* frame = anim.frames[i];
        if (!frame || frame->anchor_points.empty()) continue;
        auto& dest = frames_[i].anchors;
        if (!dest.empty()) continue;
        dest.reserve(frame->anchor_points.size());
        for (const auto& a : frame->anchor_points) {
            if (a.name.empty()) continue;
            frame_editors::FrameAnchorPoint pt;
            pt.name = a.name;
            pt.texture_x = a.texture_x;
            pt.texture_y = a.texture_y;
            pt.in_front = a.in_front;
            dest.push_back(std::move(pt));
        }
    }
    texture_primed_ = false;
}

DisplacedAssetAnchorPoint AnchorEditor::to_runtime_anchor(const frame_editors::FrameAnchorPoint& anchor) const {
    DisplacedAssetAnchorPoint runtime(anchor.name, anchor.texture_x, anchor.texture_y, anchor.in_front);
    return runtime;
}

void AnchorEditor::sync_runtime_animation_anchors(const std::string& animation_id,
                                                  const std::vector<frame_editors::AnchorFrame>& frames) {
    if (!context_.target || !context_.target->info) return;
    auto it = context_.target->info->animations.find(animation_id);
    if (it == context_.target->info->animations.end()) return;
    auto& anim = it->second;
    for (std::size_t i = 0; i < std::min(anim.frames.size(), frames.size()); ++i) {
        std::vector<DisplacedAssetAnchorPoint> pts;
        for (const auto& a : frames[i].anchors) pts.push_back(to_runtime_anchor(a));
        if (anim.frames[i]) anim.frames[i]->set_anchor_points(std::move(pts));
    }
    context_.target->mark_anchors_dirty();
}

AnchorEditor::FrameTextureInfo AnchorEditor::resolve_frame_texture(SDL_Renderer* renderer, int frame_index) const {
    FrameTextureInfo info{};
    if (frame_index < 0) {
        return info;
    }
    // Clamp frame index to known data.
    int clamped_index = frame_index;
    int known_frames = 0;
    if (!frames_.empty()) known_frames = static_cast<int>(frames_.size());
    if (known_frames > 0) clamped_index = std::clamp(clamped_index, 0, known_frames - 1);

    // Prefer runtime texture from the active in-room asset (handles atlases).
    if (context_.target && context_.target->info) {
        auto anim_it = context_.target->info->animations.find(context_.animation_id);
        if (anim_it != context_.target->info->animations.end()) {
            const auto& anim = anim_it->second;
            if (clamped_index >= 0 && clamped_index < static_cast<int>(anim.frames.size())) {
                AnimationFrame* frame = anim.frames[static_cast<std::size_t>(clamped_index)];
                if (frame && !frame->variants.empty()) {
                    const int idx = std::clamp(context_.target->current_variant_index, 0,
                                               static_cast<int>(frame->variants.size()) - 1);
                    const auto& variant = frame->variants[static_cast<std::size_t>(idx)];
                    info.texture = variant.get_base_texture();
                    if (info.texture) {
                        if (variant.uses_atlas) {
                            info.src_rect = variant.source_rect;
                            info.has_src_rect = true;
                        } else {
                            float w = 0, h = 0;
                            SDL_GetTextureSize(info.texture, &w, &h);
                            info.src_rect = SDL_Rect{0, 0, static_cast<int>(w), static_cast<int>(h)};
                            info.has_src_rect = true;
                        }
                    }
                }
            }
        }
    }

    // Fallback to baked preview frames from disk.
    int preview_count = 0;
    if (context_.preview) {
        preview_count = context_.preview->get_frame_count(context_.animation_id);
    }
    if (!info.texture && context_.preview) {
        if (!renderer && context_.assets) {
            renderer = context_.assets->renderer();
        }
        if (renderer) {
            int preview_idx = clamped_index;
            if (preview_count > 0) {
                preview_idx = std::clamp(preview_idx, 0, preview_count - 1);
            }
            SDL_Texture* preview_tex = context_.preview->get_frame_texture(renderer, context_.animation_id, preview_idx);
            if (preview_tex) {
                info.texture = preview_tex;
                float w = 0, h = 0;
                SDL_GetTextureSize(preview_tex, &w, &h);
                info.src_rect = SDL_Rect{0, 0, static_cast<int>(w), static_cast<int>(h)};
                info.has_src_rect = true;
            }
            // Last-ditch: use preview sprite sheet composite if individual frames are unavailable.
            if (!info.texture) {
                if (SDL_Texture* sheet = context_.preview->get_preview_texture(renderer, context_.animation_id)) {
                    info.texture = sheet;
                    float w = 0, h = 0;
                    SDL_GetTextureSize(sheet, &w, &h);
                    info.src_rect = SDL_Rect{0, 0, static_cast<int>(w), static_cast<int>(h)};
                    info.has_src_rect = true;
                }
            }
        }
    }

    // Final fallback: show the asset's current frame even if animation lookup failed.
    if (!info.texture && context_.target && context_.target->current_frame) {
        const AnimationFrame* frame = context_.target->current_frame;
        const int variant_idx = std::clamp(context_.target->current_variant_index,
                                           0,
                                           static_cast<int>(frame->variants.size()) - 1);
        if (variant_idx >= 0 && variant_idx < static_cast<int>(frame->variants.size())) {
            const auto& variant = frame->variants[static_cast<std::size_t>(variant_idx)];
            info.texture = variant.get_base_texture();
            if (info.texture) {
                float w = 0, h = 0;
                SDL_GetTextureSize(info.texture, &w, &h);
                info.src_rect = SDL_Rect{0, 0, static_cast<int>(w), static_cast<int>(h)};
                info.has_src_rect = true;
            }
        }
    }

    return info;
}

bool AnchorEditor::point_in_viewport(const SDL_Point& p) const { return SDL_PointInRect(&p, &viewport_rect_); }

SDL_FPoint AnchorEditor::texture_to_screen(int tx, int ty) const {
    auto dims = current_frame_dimensions();
    const float tw = static_cast<float>(dims.first);
    const float th = static_cast<float>(dims.second);
    if (tw <= 0.0f || th <= 0.0f) {
        return SDL_FPoint{static_cast<float>(viewport_rect_.x), static_cast<float>(viewport_rect_.y)};
    }
    const float origin_x = viewport_rect_.x + pan_.x + (viewport_rect_.w - tw * zoom_) * 0.5f;
    const float origin_y = viewport_rect_.y + pan_.y + (viewport_rect_.h - th * zoom_) * 0.5f;
    return SDL_FPoint{origin_x + (static_cast<float>(tx) + 0.5f) * zoom_, origin_y + (static_cast<float>(ty) + 0.5f) * zoom_};
}

bool AnchorEditor::screen_to_texture(const SDL_Point& p, int& tx, int& ty) const {
    auto dims = current_frame_dimensions();
    float tw = static_cast<float>(dims.first);
    float th = static_cast<float>(dims.second);
    if (tw <= 0.0f || th <= 0.0f) return false;
    const float origin_x = viewport_rect_.x + pan_.x + (viewport_rect_.w - tw * zoom_) * 0.5f;
    const float origin_y = viewport_rect_.y + pan_.y + (viewport_rect_.h - th * zoom_) * 0.5f;
    tx = static_cast<int>(std::floor((p.x - origin_x) / zoom_));
    ty = static_cast<int>(std::floor((p.y - origin_y) / zoom_));
    tx = std::clamp(tx, 0, std::max(0, static_cast<int>(tw) - 1));
    ty = std::clamp(ty, 0, std::max(0, static_cast<int>(th) - 1));
    return true;
}

void AnchorEditor::center_view() { pan_ = SDL_FPoint{0,0}; }

void AnchorEditor::update_drag_pick(const SDL_Point& p) {
    int tx = 0, ty = 0;
    if (!screen_to_texture(p, tx, ty)) return;
    auto& list = frames_[selected_frame_].anchors;
    if (selected_anchor_ < 0 || selected_anchor_ >= static_cast<int>(list.size())) return;
    auto& a = list[static_cast<std::size_t>(selected_anchor_)];
    if (a.texture_x != tx || a.texture_y != ty) { a.texture_x = tx; a.texture_y = ty; dirty_ = true; refresh_form(); }
}

SDL_Texture* AnchorEditor::current_frame_texture() const {
    SDL_Renderer* renderer = context_.assets ? context_.assets->renderer() : nullptr;
    return resolve_frame_texture(renderer, selected_frame_).texture;
}

} // namespace devmode::anchor_editor
