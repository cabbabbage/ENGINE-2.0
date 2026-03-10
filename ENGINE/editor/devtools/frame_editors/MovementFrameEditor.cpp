#include "MovementFrameEditor.hpp"
#include "utils/sdl_mouse_utils.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

#include "animation/animation_update.hpp"

#include "devtools/dm_styles.hpp"
#include "devtools/draw_utils.hpp"
#include "devtools/dev_mode_utils.hpp"
#include "devtools/widgets.hpp"
#include "devtools/frame_editors/shared/SnapUtils.hpp"
#include "utils/FramePointResolver.hpp"

#include "nlohmann/json.hpp"
#include "rendering/render/warped_screen_grid.hpp"
#include "utils/grid.hpp"
#include "core/axis_convention.hpp"

namespace devmode::frame_editors {

namespace {

SDL_Point round_point(const SDL_FPoint& pt) {
    return SDL_Point{static_cast<int>(std::lround(pt.x)), static_cast<int>(std::lround(pt.y))};
}

int clamp_index(int idx, int max_value) {
    if (max_value <= 0) return 0;
    return std::clamp(idx, 0, max_value - 1);
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
    point_3d_editor_ = std::make_unique<Point3DEditor>(selection_state_);
    if (selection_state_) {
        selection_state_->reset();
    }
    if (point_3d_editor_) {
        point_3d_editor_->reset_axis(AdjustmentAxis::X);
        point_3d_editor_->set_grid_resolution(context_.snap_resolution);
        // Movement mode uses raw delta Z values (like dx/dy), not percentages
        point_3d_editor_->set_z_display_mode(CoordinateDisplayMode::RawDelta);
    }
    dirty_ = false;
    wants_close_ = false;
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
    manifest_txn_.set_immediate_persist(false);
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
    frame_navigator_ = std::make_unique<FrameNavigator>();
    frame_navigator_->set_frame_count(static_cast<int>(frames_.size()));
    frame_navigator_->set_current_frame(selected_index_);
    frame_navigator_->set_on_frame_changed([this](int frame) {
        select_frame(frame);
    });
    frame_navigator_->set_on_before_change([this](int, int) {
        if (context_.on_save_and_update) {
            context_.on_save_and_update();
        }
        return true;
    });
    frame_navigator_->set_preview_source(context_.preview, context_.animation_id);
    frame_navigator_->set_on_apply_next([this]() { apply_movement_to_next_frame(); });
    frame_navigator_->set_on_apply_animation([this]() { apply_movement_to_animation(); });
    frame_navigator_->set_on_apply_all([this]() { (void)apply_movement_to_all_animations(); });
    frame_navigator_->set_on_save_and_exit([this]() {
        if (context_.on_end) {
            context_.on_end();
        }
    });

    tool_panel_ = std::make_unique<FrameToolPanel>("Movement Tool Panel", "frame_editor_tool_panel_movement");
    smooth_widget_ = std::make_unique<CheckboxWidget>(cb_smooth_.get());
    curve_widget_ = std::make_unique<CheckboxWidget>(cb_curve_.get());
    DockableCollapsible::Rows rows{
        {smooth_widget_.get()},
        {curve_widget_.get()},
    };
    if (tool_panel_) {
        tool_panel_->set_rows(rows);
        // Position set on first update when screen dimensions are available.
    }

    if (point_3d_editor_) {
        point_3d_editor_->set_on_coordinates_changed([this]() {
            if (!selection_state_) {
                return;
            }
            if (frames_.empty() || selected_index_ <= 0 ||
                selected_index_ >= static_cast<int>(frames_.size()) ||
                rel_positions_.empty() || rel_positions_z_.empty()) {
                return;
            }
            if (selected_index_ - 1 >= static_cast<int>(rel_positions_.size()) ||
                selected_index_ - 1 >= static_cast<int>(rel_positions_z_.size())) {
                return;
            }

            SDL_FPoint snapped_world = snap_world_point_to_grid(selection_state_->world_pos, context_.snap_resolution);
            float snapped_world_z = snap_world_z_to_grid(selection_state_->world_z, context_.snap_resolution);
            selection_state_->world_pos = snapped_world;
            selection_state_->world_z = snapped_world_z;

            SDL_Point anchor = asset_anchor_world();
            float base_z = base_world_z();

            float rel_x = snapped_world.x - static_cast<float>(anchor.x);
            float rel_y = snapped_world.y - static_cast<float>(anchor.y);

            SDL_FPoint prev_rel_abs = rel_positions_[selected_index_ - 1];

            frames_[selected_index_].dx = std::round(rel_x - prev_rel_abs.x);
            frames_[selected_index_].dy = std::round(rel_y - prev_rel_abs.y);
            // dz is a raw delta value (like dx/dy), not a percentage
            frames_[selected_index_].dz = std::round(snapped_world_z - base_z);

            rebuild_rel_positions();
            apply_live_changes();
            refresh_selection_state();
        });

        point_3d_editor_->set_on_point_selected([this](int index) {
            if (index < 0) {
                // Deselecting - persist changes before clearing selection state
                persist_changes();
                if (selection_state_) {
                    selection_state_->reset();
                }
            } else {
                // Only handle selection if it's the current frame's point
                if (index == selected_index_) {
                    if (selection_state_) {
                        selection_state_->target = SelectionTarget::MovementPoint;
                    }
                    if (point_3d_editor_) {
                        point_3d_editor_->set_selected_point_index(index);
                    }
                    refresh_selection_state();
                }
            }
        });

        point_3d_editor_->set_on_position_changed([this](const SDL_FPoint& new_world_pos, float new_world_z) {
            // Guard: ensure data structures exist before accessing
            if (frames_.empty() || selected_index_ <= 0 ||
                selected_index_ >= static_cast<int>(frames_.size()) ||
                rel_positions_.empty() || rel_positions_z_.empty()) {
                return;
            }

            // Guard: ensure previous position exists
            if (selected_index_ - 1 >= static_cast<int>(rel_positions_.size()) ||
                selected_index_ - 1 >= static_cast<int>(rel_positions_z_.size())) {
                return;
            }

            SDL_FPoint snapped_world = snap_world_point_to_grid(new_world_pos, context_.snap_resolution);
            float snapped_world_z = snap_world_z_to_grid(new_world_z, context_.snap_resolution);
            SDL_Point anchor = asset_anchor_world();
            float base_z = base_world_z();

            // Convert world to relative to anchor
            float rel_x = snapped_world.x - static_cast<float>(anchor.x);
            float rel_y = snapped_world.y - static_cast<float>(anchor.y);

            // Previous frame absolute relative position
            SDL_FPoint prev_rel_abs = rel_positions_[selected_index_ - 1];

            // New local delta - dz is a raw delta value (like dx/dy), not a percentage
            frames_[selected_index_].dx = std::round(rel_x - prev_rel_abs.x);
            frames_[selected_index_].dy = std::round(rel_y - prev_rel_abs.y);
            frames_[selected_index_].dz = std::round(snapped_world_z - base_z);

            rebuild_rel_positions();
            apply_live_changes();
            refresh_selection_state();
        });
    }
}

void MovementFrameEditor::end() {
    wants_close_ = false;
    dirty_ = false;
    frames_.clear();
    rel_positions_.clear();
    rel_positions_z_.clear();
    if (selection_state_) {
        selection_state_->reset();
        selection_state_ = nullptr;
    }
    point_3d_editor_ = nullptr;
    tool_panel_.reset();
    smooth_widget_.reset();
    curve_widget_.reset();
    cb_smooth_.reset();
    cb_curve_.reset();
}

bool MovementFrameEditor::handle_event(const SDL_Event& e) {
    SDL_Rect overlay_rect{0, 0, 0, 0};
    bool overlay_valid = false;
    // Handle Point3DEditor events
    if (point_3d_editor_) {
        // Use the cached container from Point3DEditor (set during render_overlays)
        // This avoids issues with SDL_GetCurrentRenderOutputSize(nullptr, ...) failing
        overlay_rect = point_3d_editor_->get_cached_container();
        overlay_valid = (overlay_rect.w > 0 && overlay_rect.h > 0);
        if (point_3d_editor_->handle_event(e, overlay_rect)) {
            return true;
        }
    }
    bool consumed = false;

    if (tool_panel_) {
        const bool prev_smooth = cb_smooth_ ? cb_smooth_->value() : smooth_enabled_;
        const bool prev_curve = cb_curve_ ? cb_curve_->value() : curve_enabled_;
        if (tool_panel_->handle_event(e)) {
            consumed = true;
        }
        if (cb_smooth_) {
            smooth_enabled_ = cb_smooth_->value();
        }
        if (cb_curve_) {
            curve_enabled_ = cb_curve_->value();
        }
        if (!smooth_enabled_) {
            curve_enabled_ = false;
            if (cb_curve_) {
                cb_curve_->set_value(false);
            }
        }
        if (smooth_enabled_ != prev_smooth || curve_enabled_ != prev_curve) {
            consumed = true;
        }
    }

    if (frame_navigator_ && frame_navigator_->handle_event(e)) {
        consumed = true;
        if (selection_state_) selection_state_->reset();
        if (point_3d_editor_) point_3d_editor_->set_selected_point_index(-1);
    }

    if (!context_.assets || !context_.target) {
        return consumed;
    }

    // Handle mouse events for 3D point manipulation
    SDL_Point mouse_pos = {0, 0};
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
        std::vector<bool> point_selectable;

        for (std::size_t i = 0; i < rel_positions_.size(); ++i) {
            SDL_FPoint screen{};
            if (project_relative_point(i, screen)) {
                point_screens.push_back(screen);
                // Only current frame's point is selectable
                point_selectable.push_back(static_cast<int>(i) == selected_index_);
            }
        }

        // Only consume event if point editor actually handled it
        consumed = point_3d_editor_->handle_mouse_event(e, point_screens, point_selectable);
    }

    return consumed;
}

void MovementFrameEditor::update(const Input& input, float) {
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
        SDL_RenderLine(renderer, static_cast<int>(std::lround(a.x)), static_cast<int>(std::lround(a.y)),
                           static_cast<int>(std::lround(b.x)), static_cast<int>(std::lround(b.y)));
    }

    for (std::size_t i = 0; i < rel_positions_.size(); ++i) {
        if (!has_screen[i]) continue;
        SDL_FPoint p = screen_points[i];
        const bool is_current_frame = (static_cast<int>(i) == selected_index_);
        const bool is_selected = (is_current_frame &&
                                 selection_state_ &&
                                 selection_state_->target == SelectionTarget::MovementPoint);
        const bool is_hovered = (static_cast<int>(i) == point_3d_editor_->get_hovered_point_index());

        if (is_current_frame) {
            point_3d_editor_->render_selectable_point(renderer, p, is_selected, is_hovered);
        } else {
            point_3d_editor_->render_non_selectable_point(renderer, p);
        }
    }
}

void MovementFrameEditor::render_overlays(SDL_Renderer* renderer) const {
    if (!renderer) return;
    layout_ui(renderer);
    if (frame_navigator_) frame_navigator_->render(renderer);
    if (tool_panel_) tool_panel_->render(renderer);

    // Render Point3DEditor overlays at the bottom
    if (point_3d_editor_) {
        int sw = 0, sh = 0;
        SDL_GetCurrentRenderOutputSize(renderer, &sw, &sh);
        int height = point_3d_editor_->get_overlay_height(sw);
        SDL_Rect bottom_container{0, sh - height, sw, height};
        point_3d_editor_->render_overlays(renderer, bottom_container);
    }
}

void MovementFrameEditor::layout_ui(SDL_Renderer* renderer) const {
    if (!renderer) return;
    int sw = 0;
    int sh = 0;
    SDL_GetCurrentRenderOutputSize(renderer, &sw, &sh);
    screen_w_ = sw;
    screen_h_ = sh;

    const int nav_height = frame_navigator_ ? frame_navigator_->get_preferred_rect().h : 0;
    nav_rect_ = SDL_Rect{0, 0, sw, nav_height};
    if (frame_navigator_) {
        frame_navigator_->set_rect(nav_rect_);
    }

    if (tool_panel_) {
        tool_panel_->set_work_area(SDL_Rect{0, 0, sw, sh});
        tool_panel_->set_position_if_unset(sw, nav_height + DMSpacing::header_gap());
    }
}


void MovementFrameEditor::select_frame(int index) {
    selected_index_ = clamp_index(index, static_cast<int>(frames_.size()));

    // Don't automatically select/refresh point - that's done explicitly when user clicks or uses arrow keys
    // Just deselect any current point when changing frames via frame navigator
    if (point_3d_editor_) {
        point_3d_editor_->set_selected_point_index(-1);
    }
    if (selection_state_) {
        selection_state_->reset();
    }

    // Update frame navigator to show correct frame
    if (frame_navigator_) {
        frame_navigator_->set_current_frame(selected_index_);
    }
}

void MovementFrameEditor::rebuild_rel_positions() {
    rel_positions_.clear();
    rel_positions_z_.clear();
    const std::size_t count = frames_.size();
    rel_positions_.resize(count, SDL_FPoint{0.0f, 0.0f});
    rel_positions_z_.resize(count, 0.0f);
    if (count == 0) return;
    // dz is a raw delta value (like dx/dy), not a percentage
    rel_positions_[0] = SDL_FPoint{0.0f, 0.0f};
    rel_positions_z_[0] = frames_[0].dz;
    for (std::size_t i = 1; i < count; ++i) {
        rel_positions_[i].x = rel_positions_[i - 1].x + frames_[i].dx;
        rel_positions_[i].y = rel_positions_[i - 1].y + frames_[i].dy;
        rel_positions_z_[i] = frames_[i].dz;
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
    // Guard: only persist if we have data
    if (frames_.empty()) {
        return;
    }
    apply_live_changes();
    refresh_selection_state();
}

void MovementFrameEditor::persist_pending_changes() {
    if (!manifest_txn_.active() || !dirty_) {
        return;
    }
    if (manifest_txn_.commit(true)) {
        dirty_ = false;
        invalidate_preview();
    }
}

void MovementFrameEditor::copy_movement_fields(MovementFrame& dest, const MovementFrame& src) const {
    dest.dx = src.dx;
    dest.dy = src.dy;
    dest.dz = src.dz;
    dest.resort_z = src.resort_z;
    dest.children = src.children;
}

void MovementFrameEditor::apply_movement_to_next_frame() {
    if (frames_.empty()) return;
    const int count = static_cast<int>(frames_.size());
    const int idx = clamp_index(selected_index_, count);
    const int target = (idx + 1) % count;
    MovementFrame src = frames_[idx];
    copy_movement_fields(frames_[target], src);
    rebuild_rel_positions();
    persist_changes();
    persist_pending_changes();
    invalidate_preview();
}

void MovementFrameEditor::apply_movement_to_animation() {
    if (frames_.empty()) return;
    const int idx = clamp_index(selected_index_, static_cast<int>(frames_.size()));
    MovementFrame src = frames_[idx];
    for (auto& f : frames_) {
        copy_movement_fields(f, src);
    }
    rebuild_rel_positions();
    persist_changes();
    persist_pending_changes();
    invalidate_preview();
}

bool MovementFrameEditor::apply_movement_to_all_animations() {
    if (!context_.document || frames_.empty()) return false;
    apply_movement_to_animation();

    const int idx = clamp_index(selected_index_, static_cast<int>(frames_.size()));
    const MovementFrame src = frames_[idx];
    const auto ids = context_.document->animation_ids();
    for (const auto& id : ids) {
        auto payload_opt = context_.document->animation_payload_json(id);
        nlohmann::json payload = payload_opt.value_or(nlohmann::json::object());
        auto frames = parse_frames_from_payload(payload);
        if (frames.empty()) {
            frames.push_back(MovementFrame{});
        }
        for (auto& f : frames) {
            copy_movement_fields(f, src);
        }
        nlohmann::json updated = build_payload_from_frames(frames, payload);
        context_.document->update_animation_payload(id, updated);
        if (context_.preview) {
            context_.preview->invalidate(id);
        }
    }
    context_.document->save_to_file_checked(true);
    return true;
}

void MovementFrameEditor::apply_live_changes() {
    dirty_ = true;
}

void MovementFrameEditor::invalidate_preview() const {
    if (context_.preview && !context_.animation_id.empty()) {
        context_.preview->invalidate(context_.animation_id);
    }
}

void MovementFrameEditor::refresh_selection_state() {
    if (!selection_state_ || !context_.assets || !context_.target) {
        return;
    }
    if (selection_state_->target != SelectionTarget::MovementPoint) {
        return;
    }
    // Movement mode uses raw delta Z, no parent height needed
    const int idx = clamp_index(selected_index_, static_cast<int>(rel_positions_.size()));
    selection_state_->child_index = -1;
    SDL_Point anchor = asset_anchor_world();
    const float base_z = base_world_z();
    SDL_FPoint world{
        static_cast<float>(anchor.x) + (idx < static_cast<int>(rel_positions_.size()) ? rel_positions_[idx].x : 0.0f),
        static_cast<float>(anchor.y) + (idx < static_cast<int>(rel_positions_.size()) ? rel_positions_[idx].y : 0.0f)
    };
    const float world_z = base_z +
        (idx < static_cast<int>(rel_positions_z_.size()) ? rel_positions_z_[idx] : 0.0f);
    SDL_FPoint screen{};
    if (!project_relative_point(static_cast<std::size_t>(idx), screen)) {
        const WarpedScreenGrid& cam = context_.camera ? *context_.camera : context_.assets->getView();
        screen = cam.map_to_screen_f(world);
    }
    selection_state_->world_pos = world;
    selection_state_->world_z = world_z;
    selection_state_->screen_pos = round_point(screen);
    selection_state_->set_anchor_world(anchor, base_z);
}

SDL_Point MovementFrameEditor::asset_anchor_world() const {
    if (!context_.target) {
        return SDL_Point{0, 0};
    }
    return animation_update::detail::bottom_middle_for(*context_.target, context_.target->world_xz_point());
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
    if (SDL_PointInRect(&pt, &nav_rect_)) return true;
    return tool_panel_ && tool_panel_->contains_point(pt);
}

}  // namespace devmode::frame_editors


