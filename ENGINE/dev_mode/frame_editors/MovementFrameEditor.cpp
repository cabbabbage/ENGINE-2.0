#include "MovementFrameEditor.hpp"

#include <SDL.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

#include "animation_update/animation_update.hpp"
#include "dev_mode/asset_sections/animation_editor_window/AnimationDocument.hpp"
#include "dev_mode/dm_styles.hpp"
#include "dev_mode/draw_utils.hpp"
#include "dev_mode/dev_mode_utils.hpp"
#include "dev_mode/widgets.hpp"
#include "dev_mode/frame_editors/shared/AxisPointRenderer.hpp"
#include "nlohmann/json.hpp"
#include "render/warped_screen_grid.hpp"
#include "utils/grid.hpp"

namespace devmode::frame_editors {

namespace {

SDL_Point round_point(const SDL_FPoint& pt) {
    return SDL_Point{static_cast<int>(std::lround(pt.x)), static_cast<int>(std::lround(pt.y))};
}

int clamp_index(int idx, int max_value) {
    if (max_value <= 0) return 0;
    return std::clamp(idx, 0, max_value - 1);
}

float parse_float(const std::string& text, float fallback) {
    try {
        return std::stof(text);
    } catch (...) {
        return fallback;
    }
}

int resolve_wheel_steps(const SDL_MouseWheelEvent& wheel) {
    float precise = wheel.preciseY;
    int delta = wheel.y;
    if (wheel.direction == SDL_MOUSEWHEEL_FLIPPED) {
        delta = -delta;
        precise = -precise;
    }
    if (std::fabs(precise) >= 0.01f) {
        return static_cast<int>(std::lround(precise));
    }
    return delta;
}

SDL_FPoint sample_quadratic_by_arclen(const SDL_FPoint& p0,
                                      const SDL_FPoint& p1,
                                      const SDL_FPoint& p2,
                                      float ratio) {
    const float t = std::clamp(ratio, 0.0f, 1.0f);
    auto lerp = [](const SDL_FPoint& a, const SDL_FPoint& b, float t) {
        return SDL_FPoint{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
    };
    SDL_FPoint a = lerp(p0, p1, t);
    SDL_FPoint b = lerp(p1, p2, t);
    return lerp(a, b, t);
}

}  // namespace

void MovementFrameEditor::begin(const FrameEditorContext& context) {
    context_ = context;
    selection_state_ = context.selection_state;
    axis_adjuster_ = context.axis_adjuster;
    if (selection_state_) {
        selection_state_->reset();
    }
    if (axis_adjuster_) {
        axis_adjuster_->reset_axis(AdjustmentAxis::X);
    }
    wants_close_ = false;
    dragging_point_ = false;
    selected_index_ = 0;
    frames_.clear();
    rel_positions_.clear();
    rel_positions_z_.clear();
    smooth_enabled_ = false;
    curve_enabled_ = false;

    if (context_.document) {
        auto payload_opt = context_.document->animation_payload_json(context_.animation_id);
        nlohmann::json payload = payload_opt.value_or(nlohmann::json::object());
        frames_ = parse_frames_from_payload(payload);
    }
    if (frames_.empty()) {
        frames_.push_back(MovementFrame{});
    }
    rebuild_rel_positions();
    refresh_selection_state();

    manifest_txn_.begin(context_);
    manifest_txn_.set_apply_callback([this]() -> bool {
        if (!context_.document) {
            return false;
        }
        auto payload_opt = context_.document->animation_payload_json(context_.animation_id);
        nlohmann::json existing = payload_opt.value_or(nlohmann::json::object());
        nlohmann::json updated = build_payload_from_frames(frames_, existing);
        return context_.document->update_animation_payload(context_.animation_id, updated);
    });

    cb_smooth_ = std::make_unique<DMCheckbox>("Smooth", smooth_enabled_);
    cb_curve_ = std::make_unique<DMCheckbox>("Curve", curve_enabled_);
    btn_back_ = std::make_unique<DMButton>("Back", &DMStyles::HeaderButton(), 80, DMButton::height());
    btn_apply_all_ = std::make_unique<DMButton>("Apply To All Frames", &DMStyles::HeaderButton(), 180, DMButton::height());
    frame_navigator_ = std::make_unique<FrameNavigator>();
    frame_navigator_->set_frame_count(static_cast<int>(frames_.size()));
    frame_navigator_->set_current_frame(selected_index_);
    frame_navigator_->set_on_frame_changed([this](int frame) {
        select_frame(frame);
    });
    tb_dx_ = std::make_unique<DMTextBox>("dx", "0");
    tb_dy_ = std::make_unique<DMTextBox>("dy", "0");
    tb_dz_ = std::make_unique<DMTextBox>("dz", "0");
    sync_text_fields();
}

void MovementFrameEditor::end() {
    if (manifest_txn_.active()) {
        manifest_txn_.commit();
    }
    wants_close_ = false;
    frames_.clear();
    rel_positions_.clear();
    rel_positions_z_.clear();
    if (selection_state_) {
        selection_state_->reset();
        selection_state_ = nullptr;
    }
    axis_adjuster_ = nullptr;
    cb_smooth_.reset();
    cb_curve_.reset();
    btn_back_.reset();
    btn_apply_all_.reset();

    tb_dx_.reset();
    tb_dy_.reset();
    tb_dz_.reset();
}

bool MovementFrameEditor::handle_event(const SDL_Event& e) {
    bool consumed = false;
    apply_text_field_changes();

    if (frame_navigator_ && frame_navigator_->handle_event(e)) {
        consumed = true;
    }
    if (btn_back_ && btn_back_->handle_event(e)) {
        wants_close_ = true;
        consumed = true;
    }
    if (btn_apply_all_ && btn_apply_all_->handle_event(e)) {
        apply_to_all_frames();
        consumed = true;
    }
    if (cb_smooth_ && cb_smooth_->handle_event(e)) {
        smooth_enabled_ = cb_smooth_->value();
        if (!smooth_enabled_) {
            curve_enabled_ = false;
            if (cb_curve_) cb_curve_->set_value(false);
        }
        consumed = true;
    }
    if (cb_curve_ && cb_curve_->handle_event(e)) {
        curve_enabled_ = smooth_enabled_ ? cb_curve_->value() : false;
        if (!smooth_enabled_) cb_curve_->set_value(false);
        consumed = true;
    }
    if (tb_dx_ && tb_dx_->handle_event(e)) {
        apply_text_field_changes();
        consumed = true;
    }
    if (tb_dy_ && tb_dy_->handle_event(e)) {
        apply_text_field_changes();
        consumed = true;
    }
    if (tb_dz_ && tb_dz_->handle_event(e)) {
        apply_text_field_changes();
        consumed = true;
    }

    if (e.type == SDL_KEYDOWN) {
        if (e.key.keysym.sym == SDLK_LEFT) {
            select_frame(selected_index_ - 1);
            consumed = true;
        } else if (e.key.keysym.sym == SDLK_RIGHT) {
            select_frame(selected_index_ + 1);
            consumed = true;
        } else if (e.key.keysym.sym == SDLK_ESCAPE) {
            wants_close_ = true;
            consumed = true;
        }
    }

    if (!context_.assets || !context_.target) {
        return consumed;
    }

    if (e.type == SDL_MOUSEWHEEL && selection_state_ && selection_state_->target == SelectionTarget::MovementPoint) {
        const int steps = resolve_wheel_steps(e.wheel);
        if (steps != 0) {
            apply_scroll_adjustment(steps);
            return true;
        }
    }

    if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
        SDL_Point mp{e.button.x, e.button.y};
        if (ui_contains_point(mp)) {
            return true;
        }
        if (auto hit = hit_test_point(mp)) {
            select_frame(*hit);
            AxisPointRenderer::cycle_axis_on_selection(selection_state_, axis_adjuster_);
            if (selection_state_) {
                selection_state_->target = SelectionTarget::MovementPoint;
                selection_state_->child_index = -1;
            }
            const int idx = clamp_index(selected_index_, static_cast<int>(rel_positions_.size()));
            drag_start_screen_ = mp;
            drag_start_rel_ = (idx < static_cast<int>(rel_positions_.size())) ? rel_positions_[idx] : SDL_FPoint{0.0f, 0.0f};
            drag_start_z_ = (idx < static_cast<int>(rel_positions_z_.size())) ? rel_positions_z_[idx] : 0.0f;
            dragging_point_ = true;
            consumed = true;
        } else if (selection_state_) {
            selection_state_->target = SelectionTarget::None;
        }
    } else if (e.type == SDL_MOUSEBUTTONUP && e.button.button == SDL_BUTTON_LEFT) {
        if (dragging_point_) {
            SDL_Point mp{e.button.x, e.button.y};
            if (!ui_contains_point(mp)) {
        SDL_FPoint desired_rel = screen_to_world_relative(mp);
        float desired_z = drag_start_z_;
        if (selection_state_) {
            const int idx = clamp_index(selected_index_, static_cast<int>(rel_positions_.size()));
            if (idx < static_cast<int>(rel_positions_.size())) {
                SDL_FPoint drag_delta{desired_rel.x - drag_start_rel_.x, desired_rel.y - drag_start_rel_.y};
                drag_delta = AxisPointRenderer::constrain_drag_delta(drag_delta, selection_state_->axis);
                desired_rel.x = drag_start_rel_.x + drag_delta.x;
                desired_rel.y = drag_start_rel_.y + drag_delta.y;

                if (selection_state_->axis == AdjustmentAxis::Z) {
                    desired_z = drag_start_z_ + drag_delta.y;
                    desired_rel.x = rel_positions_[idx].x;
                    desired_rel.y = rel_positions_[idx].y;
                } else {
                    desired_z = rel_positions_z_[idx];
                }
            }
        }
                const std::vector<SDL_FPoint> base = rel_positions_;
                const std::vector<float> base_z = rel_positions_z_;
                apply_frame_move_from_base(selected_index_, desired_rel, desired_z, base, base_z);
                rebuild_rel_positions();
                if (smooth_enabled_ && selected_index_ > 0) {
                    redistribute_frames_after_adjustment(selected_index_);
                } else {
                    persist_changes();
                }
            }
            dragging_point_ = false;
            consumed = true;
        }
    } else if (e.type == SDL_MOUSEMOTION && dragging_point_) {
        SDL_Point mp{e.motion.x, e.motion.y};
        if (ui_contains_point(mp)) {
            return true;
        }
        SDL_FPoint desired_rel = screen_to_world_relative(mp);
        float desired_z = drag_start_z_;
        if (selection_state_) {
            const int idx = clamp_index(selected_index_, static_cast<int>(rel_positions_.size()));
            if (idx < static_cast<int>(rel_positions_.size())) {
                SDL_FPoint drag_delta{desired_rel.x - drag_start_rel_.x, desired_rel.y - drag_start_rel_.y};
                drag_delta = AxisPointRenderer::constrain_drag_delta(drag_delta, selection_state_->axis);
                desired_rel.x = drag_start_rel_.x + drag_delta.x;
                desired_rel.y = drag_start_rel_.y + drag_delta.y;

                if (selection_state_->axis == AdjustmentAxis::Z) {
                    desired_z = drag_start_z_ + drag_delta.y;
                    desired_rel.x = rel_positions_[idx].x;
                    desired_rel.y = rel_positions_[idx].y;
                } else {
                    desired_z = rel_positions_z_[idx];
                }
            }
        }
        const std::vector<SDL_FPoint> base = rel_positions_;
        const std::vector<float> base_z = rel_positions_z_;
        apply_frame_move_from_base(selected_index_, desired_rel, desired_z, base, base_z);
        rebuild_rel_positions();
        if (!(smooth_enabled_ && selected_index_ > 0)) {
            persist_changes();
        }
        consumed = true;
    }

    return consumed;
}

void MovementFrameEditor::update(const Input&, float) {
    sync_text_fields();
    refresh_selection_state();
}

void MovementFrameEditor::render_world(SDL_Renderer* renderer) const {
    if (!renderer || !context_.assets || !context_.target) return;
    std::vector<SDL_FPoint> screen_points(rel_positions_.size());
    std::vector<bool> has_screen(rel_positions_.size(), false);
    for (std::size_t i = 0; i < rel_positions_.size(); ++i) {
        SDL_FPoint screen{};
        if (project_relative_point(i, screen)) {
            screen_points[i] = screen;
            has_screen[i] = true;
        }
    }
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_Color path_col = DMStyles::AccentButton().bg;
    SDL_SetRenderDrawColor(renderer, path_col.r, path_col.g, path_col.b, 205);
    for (std::size_t i = 1; i < rel_positions_.size(); ++i) {
        if (!has_screen[i - 1] || !has_screen[i]) {
            continue;
        }
        SDL_FPoint a = screen_points[i - 1];
        SDL_FPoint b = screen_points[i];
        SDL_RenderDrawLine(renderer, static_cast<int>(std::lround(a.x)), static_cast<int>(std::lround(a.y)),
                           static_cast<int>(std::lround(b.x)), static_cast<int>(std::lround(b.y)));
    }

    for (std::size_t i = 0; i < rel_positions_.size(); ++i) {
        if (!has_screen[i]) continue;
        SDL_FPoint p = screen_points[i];
        const bool is_selected = (selection_state_ && selection_state_->target == SelectionTarget::MovementPoint &&
                                 static_cast<int>(i) == selected_index_);
        const AdjustmentAxis axis = selection_state_ ? selection_state_->axis : AdjustmentAxis::X;
        AxisPointRenderer::render_axis_point(renderer, p, axis, is_selected);
    }
}

void MovementFrameEditor::render_overlays(SDL_Renderer* renderer) const {
    if (!renderer) return;
    layout_ui(renderer);
    if (btn_back_) btn_back_->render(renderer);
    if (frame_navigator_) frame_navigator_->render(renderer);
    if (cb_smooth_) cb_smooth_->render(renderer);
    if (cb_curve_) cb_curve_->render(renderer);
    if (tb_dx_) tb_dx_->render(renderer);
    if (tb_dy_) tb_dy_->render(renderer);
    if (tb_dz_) tb_dz_->render(renderer);
    if (btn_apply_all_) btn_apply_all_->render(renderer);
}

void MovementFrameEditor::layout_ui(SDL_Renderer* renderer) const {
    if (!renderer) return;
    int sw = 0;
    int sh = 0;
    SDL_GetRendererOutputSize(renderer, &sw, &sh);
    const int padding = DMSpacing::small_gap();
    const int width = 280;
    const int x = padding;
    const int y = padding;
    ui_rect_ = SDL_Rect{x, y, width, 0};
    int cursor_y = y + padding;
    int inner_w = width - padding * 2;

    auto place_row = [&](int h) -> SDL_Rect {
        SDL_Rect r{x + padding, cursor_y, inner_w, h};
        cursor_y += h + DMSpacing::small_gap();
        return r;
    };

    const int button_h = DMButton::height();
    if (btn_back_) {
        btn_back_->set_rect(place_row(button_h));
    }
    if (frame_navigator_) {
        SDL_Rect nav_rect{x + padding, cursor_y, inner_w, frame_navigator_->get_preferred_rect().h};
        frame_navigator_->set_rect(nav_rect);
        cursor_y += nav_rect.h + DMSpacing::small_gap();
    }

    if (tb_dx_ && tb_dy_ && tb_dz_) {
        int third_w = (inner_w - DMSpacing::small_gap() * 2) / 3;
        SDL_Rect dx_rect{ x + padding, cursor_y, third_w, tb_dx_->height_for_width(third_w) };
        SDL_Rect dy_rect{ x + padding + third_w + DMSpacing::small_gap(), cursor_y, third_w,
                          tb_dy_->height_for_width(third_w) };
        SDL_Rect dz_rect{ x + padding + (third_w + DMSpacing::small_gap()) * 2, cursor_y, third_w,
                          tb_dz_->height_for_width(third_w) };
        tb_dx_->set_rect(dx_rect);
        tb_dy_->set_rect(dy_rect);
        tb_dz_->set_rect(dz_rect);
        cursor_y += std::max({dx_rect.h, dy_rect.h, dz_rect.h}) + DMSpacing::small_gap();
    }

    if (cb_smooth_) {
        cb_smooth_->set_rect(place_row(DMCheckbox::height()));
    }
    if (cb_curve_) {
        cb_curve_->set_rect(place_row(DMCheckbox::height()));
    }
    if (btn_apply_all_) {
        btn_apply_all_->set_rect(place_row(DMButton::height()));
    }
    ui_rect_.h = cursor_y - y;
}

void MovementFrameEditor::sync_text_fields() {
    const int idx = clamp_index(selected_index_, static_cast<int>(frames_.size()));
    if (idx >= static_cast<int>(frames_.size())) {
        return;
    }
    const MovementFrame& frame = frames_[idx];
    const std::string dx_str = std::to_string(static_cast<int>(std::lround(frame.dx)));
    const std::string dy_str = std::to_string(static_cast<int>(std::lround(frame.dy)));
    const std::string dz_str = std::to_string(static_cast<int>(std::lround(frame.dz)));
    if (tb_dx_ && !tb_dx_->is_editing() && dx_str != last_dx_text_) {
        tb_dx_->set_value(dx_str);
        last_dx_text_ = dx_str;
    }
    if (tb_dy_ && !tb_dy_->is_editing() && dy_str != last_dy_text_) {
        tb_dy_->set_value(dy_str);
        last_dy_text_ = dy_str;
    }
    if (tb_dz_ && !tb_dz_->is_editing() && dz_str != last_dz_text_) {
        tb_dz_->set_value(dz_str);
        last_dz_text_ = dz_str;
    }
}

void MovementFrameEditor::apply_text_field_changes() {
    const int idx = clamp_index(selected_index_, static_cast<int>(frames_.size()));
    if (idx >= static_cast<int>(frames_.size())) {
        return;
    }
    bool changed = false;
    if (tb_dx_) {
        const float value = parse_float(tb_dx_->value(), frames_[idx].dx);
        if (std::fabs(value - frames_[idx].dx) > 0.001f) {
            frames_[idx].dx = value;
            changed = true;
            last_dx_text_ = tb_dx_->value();
        }
    }
    if (tb_dy_) {
        const float value = parse_float(tb_dy_->value(), frames_[idx].dy);
        if (std::fabs(value - frames_[idx].dy) > 0.001f) {
            frames_[idx].dy = value;
            changed = true;
            last_dy_text_ = tb_dy_->value();
        }
    }
    if (tb_dz_) {
        const float value = parse_float(tb_dz_->value(), frames_[idx].dz);
        if (std::fabs(value - frames_[idx].dz) > 0.001f) {
            frames_[idx].dz = value;
            changed = true;
            last_dz_text_ = tb_dz_->value();
        }
    }
    if (changed) {
        rebuild_rel_positions();
        persist_changes();
    }
}

void MovementFrameEditor::select_frame(int index) {
    selected_index_ = clamp_index(index, static_cast<int>(frames_.size()));
    dragging_point_ = false;
    refresh_selection_state();
    sync_text_fields();
}

void MovementFrameEditor::rebuild_rel_positions() {
    rel_positions_.clear();
    rel_positions_z_.clear();
    const std::size_t count = frames_.size();
    rel_positions_.resize(count, SDL_FPoint{0.0f, 0.0f});
    rel_positions_z_.resize(count, 0.0f);
    if (count == 0) return;
    rel_positions_[0] = SDL_FPoint{0.0f, 0.0f};
    rel_positions_z_[0] = 0.0f;
    for (std::size_t i = 1; i < count; ++i) {
        rel_positions_[i].x = rel_positions_[i - 1].x + frames_[i].dx;
        rel_positions_[i].y = rel_positions_[i - 1].y + frames_[i].dy;
        rel_positions_z_[i] = rel_positions_z_[i - 1] + frames_[i].dz;
    }
}

std::optional<int> MovementFrameEditor::hit_test_point(const SDL_Point& screen) const {
    if (!context_.assets || !context_.target) {
        return std::nullopt;
    }
    const float radius = 10.0f;
    const float radius_sq = radius * radius;
    for (std::size_t i = 0; i < rel_positions_.size(); ++i) {
        SDL_FPoint p{};
        if (!project_relative_point(i, p)) {
            continue;
        }
        const float dx = static_cast<float>(screen.x) - p.x;
        const float dy = static_cast<float>(screen.y) - p.y;
        if ((dx * dx + dy * dy) <= radius_sq) {
            return static_cast<int>(i);
        }
    }
    return std::nullopt;
}

void MovementFrameEditor::apply_scroll_adjustment(int steps) {
    if (frames_.empty()) return;
    const int idx = clamp_index(selected_index_, static_cast<int>(frames_.size()));
    if (idx <= 0 || idx >= static_cast<int>(rel_positions_.size())) {
        return;
    }
    SDL_FPoint desired_rel = rel_positions_[idx];
    float desired_z = rel_positions_z_[idx];
    if (selection_state_) {
        switch (selection_state_->axis) {
            case AdjustmentAxis::X:
                desired_rel.x += static_cast<float>(steps);
                break;
            case AdjustmentAxis::Y:
                desired_rel.y += static_cast<float>(steps);
                break;
            case AdjustmentAxis::Z:
                desired_z += static_cast<float>(steps);
                break;
        }
    }
    const std::vector<SDL_FPoint> base = rel_positions_;
    const std::vector<float> base_z = rel_positions_z_;
    apply_frame_move_from_base(idx, desired_rel, desired_z, base, base_z);
    rebuild_rel_positions();
    persist_changes();
}

void MovementFrameEditor::apply_frame_move_from_base(int index,
                                                     const SDL_FPoint& desired_rel,
                                                     float desired_z,
                                                     const std::vector<SDL_FPoint>& base_rel,
                                                     const std::vector<float>& base_z) {
    if (index <= 0) return;
    if (index >= static_cast<int>(frames_.size())) return;
    if (base_rel.size() != frames_.size()) return;
    if (base_z.size() != frames_.size()) return;

    frames_.front().dx = 0.0f;
    frames_.front().dy = 0.0f;
    frames_.front().dz = 0.0f;

    SDL_FPoint prev_abs = base_rel[index - 1];
    const float prev_z = base_z[index - 1];
    frames_[index].dx = static_cast<float>(std::round(desired_rel.x - prev_abs.x));
    frames_[index].dy = static_cast<float>(std::round(desired_rel.y - prev_abs.y));
    frames_[index].dz = static_cast<float>(std::round(desired_z - prev_z));

    SDL_FPoint last_abs = desired_rel;
    float last_z = desired_z;
    for (int j = index + 1; j < static_cast<int>(frames_.size()); ++j) {
        const SDL_FPoint desired = base_rel[j];
        frames_[j].dx = static_cast<float>(std::round(desired.x - last_abs.x));
        frames_[j].dy = static_cast<float>(std::round(desired.y - last_abs.y));
        frames_[j].dz = static_cast<float>(std::round(base_z[j] - last_z));
        last_abs = desired;
        last_z = base_z[j];
    }
}

void MovementFrameEditor::redistribute_frames_after_adjustment(int adjusted_index) {
    const size_t count = frames_.size();
    if (count < 3) {
        persist_changes();
        return;
    }
    const int last_index = static_cast<int>(count) - 1;
    if (adjusted_index <= 0) {
        persist_changes();
        return;
    }
    if (rel_positions_.size() != count) {
        rebuild_rel_positions();
    }
    if (rel_positions_.size() != count) {
        persist_changes();
        return;
    }

    const std::vector<SDL_FPoint> original_positions = rel_positions_;
    std::vector<SDL_FPoint> redistributed = original_positions;
    if (curve_enabled_) {
        apply_curved_smoothing(adjusted_index, original_positions, redistributed, last_index);
    } else {
        apply_linear_smoothing(adjusted_index, redistributed, last_index);
    }

    frames_[0].dx = 0.0f;
    frames_[0].dy = 0.0f;
    frames_[0].dz = 0.0f;
    for (size_t i = 1; i < count; ++i) {
        const SDL_FPoint prev = redistributed[i - 1];
        const SDL_FPoint curr = redistributed[i];
        frames_[i].dx = static_cast<float>(std::round(curr.x - prev.x));
        frames_[i].dy = static_cast<float>(std::round(curr.y - prev.y));
    }
    rebuild_rel_positions();
    persist_changes();
}

void MovementFrameEditor::apply_linear_smoothing(int adjusted_index,
                                                 std::vector<SDL_FPoint>& redistributed,
                                                 int last_index) const {
    if (redistributed.empty()) return;
    if (adjusted_index <= 0) return;
    const SDL_FPoint start = redistributed.front();
    const SDL_FPoint end = redistributed.back();
    const float steps = static_cast<float>(last_index);
    if (steps <= 0.0f) {
        return;
    }
    if (adjusted_index >= 1 && adjusted_index < last_index) {
        const SDL_FPoint anchor = redistributed[adjusted_index];
        const float pre_steps = static_cast<float>(adjusted_index);
        const SDL_FPoint pre_delta{anchor.x - start.x, anchor.y - start.y};
        for (int j = 1; j < adjusted_index; ++j) {
            const float t = pre_steps > 0.0f ? (static_cast<float>(j) / pre_steps) : 0.0f;
            SDL_FPoint new_pos{start.x + pre_delta.x * t, start.y + pre_delta.y * t};
            redistributed[j] = new_pos;
        }
        const float post_steps = static_cast<float>(last_index - adjusted_index);
        const SDL_FPoint post_delta{end.x - anchor.x, end.y - anchor.y};
        for (int j = adjusted_index + 1; j < last_index; ++j) {
            const float u = post_steps > 0.0f ? (static_cast<float>(j - adjusted_index) / post_steps) : 0.0f;
            SDL_FPoint new_pos{anchor.x + post_delta.x * u, anchor.y + post_delta.y * u};
            redistributed[j] = new_pos;
        }
    } else {
        const SDL_FPoint delta{end.x - start.x, end.y - start.y};
        for (int j = 1; j < last_index; ++j) {
            const float t = static_cast<float>(j) / steps;
            SDL_FPoint new_pos{start.x + delta.x * t, start.y + delta.y * t};
            redistributed[j] = new_pos;
        }
    }
}

void MovementFrameEditor::apply_curved_smoothing(int adjusted_index,
                                                 const std::vector<SDL_FPoint>& original,
                                                 std::vector<SDL_FPoint>& redistributed,
                                                 int last_index) const {
    if (redistributed.size() < 2) return;
    if (original.size() != redistributed.size()) return;
    if (adjusted_index <= 0) return;

    auto clamp_control = [](const SDL_FPoint& p0, const SDL_FPoint& p2, SDL_FPoint& control) {
        SDL_FPoint midpoint{(p0.x + p2.x) * 0.5f, (p0.y + p2.y) * 0.5f};
        float dx = control.x - midpoint.x;
        float dy = control.y - midpoint.y;
        float dist = std::sqrt(dx * dx + dy * dy);
        const float span = std::sqrt((p2.x - p0.x) * (p2.x - p0.x) + (p2.y - p0.y) * (p2.y - p0.y));
        const float max_offset = std::clamp(span * 0.45f, 0.0f, 160.0f);
        if (dist > max_offset && dist > 0.0f) {
            const float scale = max_offset / dist;
            control.x = midpoint.x + dx * scale;
            control.y = midpoint.y + dy * scale;
            dist = max_offset;
        }
        if (dist < 1.0f && span > 0.0f) {
            const float nx = -(p2.y - p0.y) / span;
            const float ny = (p2.x - p0.x) / span;
            const float offset = std::min(span * 0.2f, 40.0f);
            control.x = midpoint.x + nx * offset;
            control.y = midpoint.y + ny * offset;
        }
    };

    auto place_half = [&](int first_idx, int second_idx) {
        const int segment_count = second_idx - first_idx;
        if (segment_count <= 1) return;
        SDL_FPoint p0 = redistributed[first_idx];
        SDL_FPoint p2 = redistributed[second_idx];
        SDL_FPoint control{(p0.x + p2.x) * 0.5f, (p0.y + p2.y) * 0.5f};
        const int interior_count = segment_count - 1;
        if (interior_count > 0) {
            int mid_index = first_idx + (segment_count / 2);
            mid_index = std::clamp(mid_index, first_idx + 1, second_idx - 1);
            if (mid_index >= 0 && mid_index < static_cast<int>(original.size())) {
                control = original[mid_index];
            }
        }
        clamp_control(p0, p2, control);
        for (int j = first_idx + 1; j < second_idx; ++j) {
            const float ratio = static_cast<float>(j - first_idx) / static_cast<float>(segment_count);
            redistributed[j] = sample_quadratic_by_arclen(p0, control, p2, ratio);
        }
    };

    place_half(0, std::min(adjusted_index, last_index));
    if (adjusted_index < last_index) {
        place_half(adjusted_index, last_index);
    }
}

void MovementFrameEditor::persist_changes() {
    if (manifest_txn_.active()) {
        manifest_txn_.commit();
    }
    refresh_selection_state();
    sync_text_fields();
}

void MovementFrameEditor::apply_to_all_frames() {
    if (frames_.empty()) return;
    const int idx = clamp_index(selected_index_, static_cast<int>(frames_.size()));
    MovementFrame src = frames_[idx];
    for (size_t i = 1; i < frames_.size(); ++i) {
        frames_[i].dx = src.dx;
        frames_[i].dy = src.dy;
        frames_[i].dz = src.dz;
        frames_[i].resort_z = src.resort_z;
    }
    rebuild_rel_positions();
    persist_changes();
}

void MovementFrameEditor::refresh_selection_state() {
    if (!selection_state_ || !context_.assets || !context_.target) {
        return;
    }
    if (selection_state_->target != SelectionTarget::MovementPoint) {
        return;
    }
    const int idx = clamp_index(selected_index_, static_cast<int>(rel_positions_.size()));
    selection_state_->child_index = -1;
    SDL_Point anchor = asset_anchor_world();
    SDL_FPoint world{
        static_cast<float>(anchor.x) + (idx < static_cast<int>(rel_positions_.size()) ? rel_positions_[idx].x : 0.0f),
        static_cast<float>(anchor.y) + (idx < static_cast<int>(rel_positions_.size()) ? rel_positions_[idx].y : 0.0f)
    };
    const float world_z = base_world_z() +
        (idx < static_cast<int>(rel_positions_z_.size()) ? rel_positions_z_[idx] : 0.0f);
    SDL_FPoint screen{};
    if (!project_relative_point(static_cast<std::size_t>(idx), screen)) {
        const WarpedScreenGrid& cam = context_.camera ? *context_.camera : context_.assets->getView();
        screen = cam.map_to_screen_f(world);
    }
    selection_state_->world_pos = world;
    selection_state_->world_z = world_z;
    selection_state_->screen_pos = round_point(screen);
}

SDL_Point MovementFrameEditor::asset_anchor_world() const {
    if (!context_.target) {
        return SDL_Point{0, 0};
    }
    return animation_update::detail::bottom_middle_for(*context_.target, context_.target->pos);
}

float MovementFrameEditor::base_world_z() const {
    return context_.target ? context_.target->world_z_offset() : 0.0f;
}

bool MovementFrameEditor::project_relative_point(std::size_t idx, SDL_FPoint& out) const {
    if (!context_.assets || !context_.target) {
        return false;
    }
    if (idx >= rel_positions_.size() || idx >= rel_positions_z_.size()) {
        return false;
    }
    const WarpedScreenGrid& cam = context_.camera ? *context_.camera : context_.assets->getView();
    SDL_Point anchor = asset_anchor_world();
    SDL_FPoint world{
        rel_positions_[idx].x + static_cast<float>(anchor.x),
        rel_positions_[idx].y + static_cast<float>(anchor.y)
    };
    const float world_z = base_world_z() + rel_positions_z_[idx];
    if (cam.project_world_point(world, world_z, out)) {
        return true;
    }
    out = cam.map_to_screen_f(world);
    return true;
}

SDL_FPoint MovementFrameEditor::screen_to_world_relative(const SDL_Point& screen) const {
    SDL_FPoint rel{0.0f, 0.0f};
    if (!context_.assets || !context_.target) {
        return rel;
    }
    const WarpedScreenGrid& cam = context_.camera ? *context_.camera : context_.assets->getView();
    SDL_FPoint world = cam.screen_to_map(screen);
    SDL_Point anchor = asset_anchor_world();
    SDL_Point world_px{static_cast<int>(std::lround(world.x)), static_cast<int>(std::lround(world.y))};
    int snap_r = vibble::grid::clamp_resolution(std::max(0, context_.snap_resolution));
    SDL_Point snapped = vibble::grid::snap_world_to_vertex(world_px, snap_r);
    rel.x = static_cast<float>(snapped.x - anchor.x);
    rel.y = static_cast<float>(snapped.y - anchor.y);
    return rel;
}

bool MovementFrameEditor::ui_contains_point(const SDL_Point& pt) const {
    return SDL_PointInRect(&pt, &ui_rect_) == SDL_TRUE;
}

}  // namespace devmode::frame_editors
