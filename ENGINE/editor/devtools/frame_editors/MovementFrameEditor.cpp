#include "MovementFrameEditor.hpp"
#include "utils/sdl_mouse_utils.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
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

int wrap_index(int idx, int count) {
    if (count <= 0) return 0;
    const int mod = idx % count;
    return mod < 0 ? mod + count : mod;
}

SDL_FPoint sample_quadratic_by_arclen(const SDL_FPoint& p0,
                                      const SDL_FPoint& p1,
                                      const SDL_FPoint& p2,
                                      float ratio) {
    auto sample = [](const SDL_FPoint& a, const SDL_FPoint& b, const SDL_FPoint& c, float t) {
        const float clamped = std::clamp(t, 0.0f, 1.0f);
        auto lerp = [](const SDL_FPoint& lhs, const SDL_FPoint& rhs, float t_val) {
            return SDL_FPoint{lhs.x + (rhs.x - lhs.x) * t_val, lhs.y + (rhs.y - lhs.y) * t_val};
        };
        const SDL_FPoint p01 = lerp(a, b, clamped);
        const SDL_FPoint p12 = lerp(b, c, clamped);
        return lerp(p01, p12, clamped);
    };

    constexpr int kSamples = 32;
    const float target_ratio = std::clamp(ratio, 0.0f, 1.0f);
    if (target_ratio <= 0.0f) {
        return p0;
    }
    if (target_ratio >= 1.0f) {
        return p2;
    }

    std::array<float, kSamples + 1> cumulative{};
    SDL_FPoint prev = sample(p0, p1, p2, 0.0f);
    float total = 0.0f;
    cumulative[0] = 0.0f;
    for (int i = 1; i <= kSamples; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(kSamples);
        const SDL_FPoint current = sample(p0, p1, p2, t);
        const float dx = current.x - prev.x;
        const float dy = current.y - prev.y;
        total += std::sqrt(dx * dx + dy * dy);
        cumulative[static_cast<std::size_t>(i)] = total;
        prev = current;
    }
    if (total <= 1e-5f) {
        return sample(p0, p1, p2, target_ratio);
    }

    const float target = target_ratio * total;
    float resolved_t = target_ratio;
    for (int i = 1; i <= kSamples; ++i) {
        const float end = cumulative[static_cast<std::size_t>(i)];
        if (target > end) {
            continue;
        }
        const float start = cumulative[static_cast<std::size_t>(i - 1)];
        const float segment = std::max(1e-5f, end - start);
        const float local = (target - start) / segment;
        const float t0 = static_cast<float>(i - 1) / static_cast<float>(kSamples);
        const float t1 = static_cast<float>(i) / static_cast<float>(kSamples);
        resolved_t = t0 + (t1 - t0) * local;
        break;
    }
    return sample(p0, p1, p2, resolved_t);
}

}  // namespace

void MovementFrameEditor::begin(const FrameEditorContext& context) {
    context_ = context;
    had_previous_static_frame_ = false;
    previous_static_frame_ = false;
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
    selected_path_index_ = 0;
    selected_index_ = 0;
    movement_paths_.clear();
    frames_.clear();
    rel_positions_.clear();
    rel_positions_z_.clear();
    smooth_enabled_ = false;
    curve_enabled_ = false;

    if (context_.document) {
        auto payload_opt = context_.document->animation_payload_json(context_.animation_id);
        nlohmann::json payload = payload_opt.value_or(nlohmann::json::object());
        movement_paths_ = parse_movement_paths_from_payload(payload);
    }
    if (movement_paths_.empty()) {
        movement_paths_.push_back(std::vector<MovementFrame>{MovementFrame{}});
    }
    if (movement_paths_.front().empty()) {
        movement_paths_.front().push_back(MovementFrame{});
    }
    frames_ = movement_paths_.front();
    if (frames_.empty()) {
        frames_.push_back(MovementFrame{});
    }
    if (context_.target) {
        had_previous_static_frame_ = true;
        previous_static_frame_ = context_.target->static_frame;
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
        sync_current_path_from_frames();
        nlohmann::json updated = build_payload_from_movement_paths(movement_paths_, existing);
        return context_.document->update_animation_payload(context_.animation_id, updated);
    });

    cb_smooth_ = std::make_unique<DMCheckbox>("Smooth", smooth_enabled_);
    cb_curve_ = std::make_unique<DMCheckbox>("Curve", curve_enabled_);
    btn_prev_path_ = std::make_unique<DMButton>("< Path", &DMStyles::SecondaryButton(), 100, DMButton::height());
    btn_path_label_ = std::make_unique<DMButton>("Path 1/1", &DMStyles::HeaderButton(), 120, DMButton::height());
    btn_next_path_ = std::make_unique<DMButton>("Path >", &DMStyles::SecondaryButton(), 100, DMButton::height());
    btn_add_path_ = std::make_unique<DMButton>("Add Path", &DMStyles::CreateButton(), 120, DMButton::height());
    btn_delete_path_ = std::make_unique<DMButton>("Delete Path", &DMStyles::DeleteButton(), 120, DMButton::height());
    frame_navigator_ = std::make_unique<FrameNavigator>();
    frame_navigator_->set_parent_window(context_.parent_window);
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
    prev_path_widget_ = std::make_unique<ButtonWidget>(btn_prev_path_.get(), [this]() {
        select_path(selected_path_index_ - 1);
    });
    path_label_widget_ = std::make_unique<ButtonWidget>(btn_path_label_.get(), []() {});
    next_path_widget_ = std::make_unique<ButtonWidget>(btn_next_path_.get(), [this]() {
        select_path(selected_path_index_ + 1);
    });
    add_path_widget_ = std::make_unique<ButtonWidget>(btn_add_path_.get(), [this]() {
        add_movement_path();
    });
    delete_path_widget_ = std::make_unique<ButtonWidget>(btn_delete_path_.get(), [this]() {
        delete_selected_movement_path();
    });
    smooth_widget_ = std::make_unique<CheckboxWidget>(cb_smooth_.get());
    curve_widget_ = std::make_unique<CheckboxWidget>(cb_curve_.get());
    DockableCollapsible::Rows rows{
        {prev_path_widget_.get(), path_label_widget_.get(), next_path_widget_.get()},
        {add_path_widget_.get(), delete_path_widget_.get()},
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
            if (frames_.empty() || selected_index_ < 0 ||
                selected_index_ >= static_cast<int>(frames_.size()) ||
                rel_positions_.empty() || rel_positions_z_.empty()) {
                return;
            }
            const int previous_index = selected_index_ - 1;
            if (previous_index >= static_cast<int>(rel_positions_.size()) ||
                previous_index >= static_cast<int>(rel_positions_z_.size())) {
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

            const SDL_FPoint prev_rel_abs =
                (previous_index >= 0) ? rel_positions_[previous_index] : SDL_FPoint{0.0f, 0.0f};
            const float prev_rel_z = (previous_index >= 0) ? rel_positions_z_[previous_index] : 0.0f;

            frames_[selected_index_].dx = std::round(rel_x - prev_rel_abs.x);
            frames_[selected_index_].dy = std::round(rel_y - prev_rel_abs.y);
            // dz is a per-frame delta.
            frames_[selected_index_].dz = std::round((snapped_world_z - base_z) - prev_rel_z);

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
            if (frames_.empty() || selected_index_ < 0 ||
                selected_index_ >= static_cast<int>(frames_.size()) ||
                rel_positions_.empty() || rel_positions_z_.empty()) {
                return;
            }

            const int previous_index = selected_index_ - 1;
            if (previous_index >= static_cast<int>(rel_positions_.size()) ||
                previous_index >= static_cast<int>(rel_positions_z_.size())) {
                return;
            }

            SDL_FPoint snapped_world = snap_world_point_to_grid(new_world_pos, context_.snap_resolution);
            float snapped_world_z = snap_world_z_to_grid(new_world_z, context_.snap_resolution);
            SDL_Point anchor = asset_anchor_world();
            float base_z = base_world_z();

            // Convert world to relative to anchor
            float rel_x = snapped_world.x - static_cast<float>(anchor.x);
            float rel_y = snapped_world.y - static_cast<float>(anchor.y);

            const SDL_FPoint prev_rel_abs =
                (previous_index >= 0) ? rel_positions_[previous_index] : SDL_FPoint{0.0f, 0.0f};
            const float prev_rel_z = (previous_index >= 0) ? rel_positions_z_[previous_index] : 0.0f;

            // New local per-frame deltas.
            frames_[selected_index_].dx = std::round(rel_x - prev_rel_abs.x);
            frames_[selected_index_].dy = std::round(rel_y - prev_rel_abs.y);
            frames_[selected_index_].dz = std::round((snapped_world_z - base_z) - prev_rel_z);

            rebuild_rel_positions();
            apply_live_changes();
            refresh_selection_state();
        });
    }

    if (selection_state_) {
        selection_state_->target = SelectionTarget::MovementPoint;
    }
    if (point_3d_editor_) {
        point_3d_editor_->set_selected_point_index(selected_index_);
    }
    refresh_selection_state();
    update_path_button_labels();

    apply_selected_frame_to_target();
}

void MovementFrameEditor::end() {
    if (had_previous_static_frame_ && context_.target) {
        context_.target->static_frame = previous_static_frame_;
        if (context_.assets) {
            context_.assets->mark_active_assets_dirty();
        }
    }
    had_previous_static_frame_ = false;
    previous_static_frame_ = false;
    wants_close_ = false;
    dirty_ = false;
    movement_paths_.clear();
    frames_.clear();
    rel_positions_.clear();
    rel_positions_z_.clear();
    if (selection_state_) {
        selection_state_->reset();
        selection_state_ = nullptr;
    }
    point_3d_editor_ = nullptr;
    tool_panel_.reset();
    prev_path_widget_.reset();
    path_label_widget_.reset();
    next_path_widget_.reset();
    add_path_widget_.reset();
    delete_path_widget_.reset();
    smooth_widget_.reset();
    curve_widget_.reset();
    btn_prev_path_.reset();
    btn_path_label_.reset();
    btn_next_path_.reset();
    btn_add_path_.reset();
    btn_delete_path_.reset();
    cb_smooth_.reset();
    cb_curve_.reset();
}

bool MovementFrameEditor::handle_event(const SDL_Event& e) {
    SDL_Rect overlay_rect{0, 0, 0, 0};
    bool overlay_valid = false;
    // Cache Point3DEditor overlay bounds for hit-testing and deferred input handling.
    if (point_3d_editor_) {
        overlay_rect = point_3d_editor_->get_cached_container();
        overlay_valid = (overlay_rect.w > 0 && overlay_rect.h > 0);
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

    // Let the tool panel consume clicks first so path buttons remain clickable even
    // when point-editor textboxes currently have focus.
    if (!consumed && point_3d_editor_ && point_3d_editor_->handle_event(e, overlay_rect)) {
        consumed = true;
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

        // Only consume event if point editor actually handled it, but do not
        // clear an already-consumed state from earlier UI handlers (tool panel,
        // frame navigator, etc).
        if (point_3d_editor_) {
            consumed = point_3d_editor_->handle_mouse_event(e, point_screens, point_selectable) || consumed;
        }
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

    if (selection_state_) {
        selection_state_->target = SelectionTarget::MovementPoint;
    }
    if (point_3d_editor_) {
        point_3d_editor_->set_selected_point_index(selected_index_);
    }
    refresh_selection_state();

    // Update frame navigator to show correct frame
    if (frame_navigator_) {
        frame_navigator_->set_current_frame(selected_index_);
    }

    apply_selected_frame_to_target();
}

void MovementFrameEditor::rebuild_rel_positions() {
    rel_positions_.clear();
    rel_positions_z_.clear();
    const std::size_t count = frames_.size();
    rel_positions_.resize(count, SDL_FPoint{0.0f, 0.0f});
    rel_positions_z_.resize(count, 0.0f);
    if (count == 0) return;
    // Each frame stores per-frame deltas. Relative positions are cumulative.
    rel_positions_[0] = SDL_FPoint{frames_[0].dx, frames_[0].dy};
    rel_positions_z_[0] = frames_[0].dz;
    for (std::size_t i = 1; i < count; ++i) {
        rel_positions_[i].x = rel_positions_[i - 1].x + frames_[i].dx;
        rel_positions_[i].y = rel_positions_[i - 1].y + frames_[i].dy;
        rel_positions_z_[i] = rel_positions_z_[i - 1] + frames_[i].dz;
    }
}

void MovementFrameEditor::redistribute_frames_after_adjustment(int adjusted_index) {
    const size_t count = frames_.size();
    if (count < 3) {
        persist_changes();
        return;
    }
    const int last_index = static_cast<int>(count) - 1;
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

    SDL_FPoint prev{0.0f, 0.0f};
    for (size_t i = 0; i < count; ++i) {
        const SDL_FPoint curr = redistributed[i];
        frames_[i].dx = static_cast<float>(std::round(curr.x - prev.x));
        frames_[i].dy = static_cast<float>(std::round(curr.y - prev.y));
        prev = curr;
    }
    rebuild_rel_positions();
    persist_changes();
}

void MovementFrameEditor::apply_linear_smoothing(int adjusted_index,
                                                 std::vector<SDL_FPoint>& redistributed,
                                                 int last_index) const {
    if (redistributed.empty()) return;
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
        const SDL_FPoint midpoint{(p0.x + p2.x) * 0.5f, (p0.y + p2.y) * 0.5f};
        control.x = midpoint.x + (control.x - midpoint.x) * 1.35f;
        control.y = midpoint.y + (control.y - midpoint.y) * 1.35f;
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
    sync_current_path_from_frames();
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
    dest.rotation_degrees = src.rotation_degrees;
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
        auto paths = parse_movement_paths_from_payload(payload);
        if (paths.empty()) {
            paths.push_back(std::vector<MovementFrame>{MovementFrame{}});
        }
        for (auto& path : paths) {
            if (path.empty()) {
                path.push_back(MovementFrame{});
            }
            for (auto& f : path) {
                copy_movement_fields(f, src);
            }
        }
        nlohmann::json updated = build_payload_from_movement_paths(paths, payload);
        context_.document->update_animation_payload(id, updated);
        if (context_.preview) {
            context_.preview->invalidate(id);
        }
    }
    context_.document->save_to_file_checked(true);
    return true;
}

void MovementFrameEditor::apply_live_changes() {
    sync_current_path_from_frames();
    dirty_ = true;
}

void MovementFrameEditor::invalidate_preview() const {
    if (context_.preview && !context_.animation_id.empty()) {
        context_.preview->invalidate(context_.animation_id);
    }
}

void MovementFrameEditor::sync_current_path_from_frames() {
    if (movement_paths_.empty()) {
        movement_paths_.push_back(std::vector<MovementFrame>{MovementFrame{}});
    }
    selected_path_index_ = clamp_index(selected_path_index_, static_cast<int>(movement_paths_.size()));
    if (frames_.empty()) {
        frames_.push_back(MovementFrame{});
    }
    movement_paths_[static_cast<std::size_t>(selected_path_index_)] = frames_;
}

void MovementFrameEditor::select_path(int index) {
    if (movement_paths_.empty()) {
        movement_paths_.push_back(std::vector<MovementFrame>{MovementFrame{}});
    }

    // Persist edits on the currently active path before loading a different path.
    sync_current_path_from_frames();
    persist_pending_changes();

    const int count = static_cast<int>(movement_paths_.size());
    const int next_path_index = wrap_index(index, count);
    if (next_path_index == selected_path_index_ && !frames_.empty()) {
        update_path_button_labels();
        return;
    }

    selected_path_index_ = next_path_index;
    frames_ = movement_paths_[static_cast<std::size_t>(selected_path_index_)];
    if (frames_.empty()) {
        frames_.push_back(MovementFrame{});
    }
    selected_index_ = clamp_index(selected_index_, static_cast<int>(frames_.size()));
    rebuild_rel_positions();
    refresh_selection_state();
    if (frame_navigator_) {
        frame_navigator_->set_frame_count(static_cast<int>(frames_.size()));
        frame_navigator_->set_current_frame(selected_index_);
    }
    update_path_button_labels();
    apply_selected_frame_to_target();
    invalidate_preview();
}

void MovementFrameEditor::add_movement_path() {
    sync_current_path_from_frames();
    std::vector<MovementFrame> new_path(frames_.size(), MovementFrame{});
    if (new_path.empty()) {
        new_path.push_back(MovementFrame{});
    }
    movement_paths_.push_back(std::move(new_path));
    select_path(static_cast<int>(movement_paths_.size()) - 1);
    persist_changes();
    persist_pending_changes();
    invalidate_preview();
}

void MovementFrameEditor::delete_selected_movement_path() {
    if (movement_paths_.size() <= 1) {
        return;
    }
    selected_path_index_ = clamp_index(selected_path_index_, static_cast<int>(movement_paths_.size()));
    movement_paths_.erase(movement_paths_.begin() + selected_path_index_);
    selected_path_index_ = clamp_index(selected_path_index_, static_cast<int>(movement_paths_.size()));
    frames_ = movement_paths_[static_cast<std::size_t>(selected_path_index_)];
    if (frames_.empty()) {
        frames_.push_back(MovementFrame{});
    }
    selected_index_ = clamp_index(selected_index_, static_cast<int>(frames_.size()));
    rebuild_rel_positions();
    refresh_selection_state();
    if (frame_navigator_) {
        frame_navigator_->set_frame_count(static_cast<int>(frames_.size()));
        frame_navigator_->set_current_frame(selected_index_);
    }
    update_path_button_labels();
    persist_changes();
    persist_pending_changes();
    invalidate_preview();
}

void MovementFrameEditor::update_path_button_labels() {
    const int count = std::max(1, static_cast<int>(movement_paths_.size()));
    selected_path_index_ = clamp_index(selected_path_index_, count);
    if (btn_path_label_) {
        btn_path_label_->set_text("Path " + std::to_string(selected_path_index_ + 1) + "/" + std::to_string(count));
    }
    if (btn_prev_path_) {
        btn_prev_path_->set_text("< Path");
    }
    if (btn_next_path_) {
        btn_next_path_->set_text("Path >");
    }
    if (btn_delete_path_) {
        btn_delete_path_->set_text(count > 1 ? "Delete Path" : "Delete Path (min 1)");
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

void MovementFrameEditor::apply_selected_frame_to_target() {
    if (!context_.target || !context_.target->info || context_.animation_id.empty()) {
        return;
    }

    context_.target->set_current_animation(context_.animation_id);
    auto anim_it = context_.target->info->animations.find(context_.target->current_animation);
    if (anim_it == context_.target->info->animations.end()) {
        anim_it = context_.target->info->animations.find(context_.animation_id);
    }
    if (anim_it == context_.target->info->animations.end() || !anim_it->second.has_frames()) {
        return;
    }

    const int frame_count = static_cast<int>(anim_it->second.frame_count());
    if (frame_count <= 0) {
        return;
    }

    const int frame_index = clamp_index(selected_index_, frame_count);
    AnimationFrame* frame = anim_it->second.primary_frame_at(static_cast<std::size_t>(frame_index));
    if (!frame) {
        frame = anim_it->second.get_first_frame();
    }
    if (!frame) {
        return;
    }

    context_.target->current_animation = anim_it->first;
    context_.target->current_frame = frame;
    context_.target->set_frame_progress(0.0f);
    context_.target->static_frame = true;
    context_.target->refresh_frame_texture_bindings();
    if (context_.assets) {
        context_.assets->mark_active_assets_dirty();
    }
}

}  // namespace devmode::frame_editors
