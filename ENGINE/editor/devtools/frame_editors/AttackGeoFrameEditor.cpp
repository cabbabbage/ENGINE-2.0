#include "AttackGeoFrameEditor.hpp"
#include "utils/sdl_mouse_utils.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <utility>

#include "animation/animation_update.hpp"
#include "devtools/asset_editor/animation_editor_window/AnimationDocument.hpp"
#include "devtools/dm_styles.hpp"
#include "devtools/draw_utils.hpp"
#include "devtools/dev_mode_utils.hpp"
#include "devtools/widgets.hpp"
#include "devtools/frame_editors/shared/SnapUtils.hpp"
#include "utils/FramePointResolver.hpp"

#include "nlohmann/json.hpp"
#include "rendering/render/warped_screen_grid.hpp"

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

int parse_int(const std::string& text, int fallback) {
    try {
        return std::stoi(text);
    } catch (...) {
        return fallback;
    }
}

int resolve_wheel_steps(const SDL_MouseWheelEvent& wheel) {
    float precise = wheel.y;
    int delta = wheel.integer_y;
    if (wheel.direction == SDL_MOUSEWHEEL_FLIPPED) {
        delta = -delta;
        precise = -precise;
    }
    if (std::fabs(precise) >= 0.01f) {
        return static_cast<int>(std::lround(precise));
    }
    return delta;
}

}  // namespace

void AttackGeoFrameEditor::begin(const FrameEditorContext& context) {
    context_ = context;
    selection_state_ = context.selection_state;
    point_3d_editor_ = std::make_unique<Point3DEditor>(selection_state_);
    if (selection_state_) {
        selection_state_->reset();
    }
    if (point_3d_editor_) {
        point_3d_editor_->reset_axis(AdjustmentAxis::X);
        point_3d_editor_->set_grid_resolution(context_.snap_resolution);
        // Attack geometry uses Z as percentage of parent height
        point_3d_editor_->set_z_display_mode(CoordinateDisplayMode::Percentage);
        FramePointResolver resolver(context_.target);
        point_3d_editor_->set_parent_height(resolver.parent_height_px());
        point_3d_editor_->set_on_coordinates_changed([this]() {
            auto* vec = current_attack_vector();
            if (!vec || !selection_state_) {
                return;
            }
            SDL_FPoint snapped_world = snap_world_point_to_grid(selection_state_->world_pos, context_.snap_resolution);
            float snapped_world_z = snap_world_z_to_grid(selection_state_->world_z, context_.snap_resolution);
            selection_state_->world_pos = snapped_world;
            selection_state_->world_z = snapped_world_z;
            SDL_Point anchor = asset_anchor_world();
            float scale = asset_local_scale();
            FramePointResolver resolver(context_.target);
            SDL_FPoint local;
            local.x = (snapped_world.x - static_cast<float>(anchor.x)) / scale;
            local.y = (static_cast<float>(anchor.y) - snapped_world.y) / scale;
            float x_percent = resolver.to_percent_xy(local.x);
            float y_percent = resolver.to_percent_xy(local.y);
            float depth_percent = resolver.to_percent_depth(snapped_world_z);
            switch (selected_handle_) {
                case AttackHandle::Start:
                    vec->start_x = x_percent;
                    vec->start_y = y_percent;
                    vec->start_z = depth_percent;
                    break;
                case AttackHandle::Control:
                    vec->control_x = x_percent;
                    vec->control_y = y_percent;
                    vec->control_z = depth_percent;
                    break;
                case AttackHandle::End:
                    vec->end_x = x_percent;
                    vec->end_y = y_percent;
                    vec->end_z = depth_percent;
                    break;
                default:
                    break;
            }
            refresh_attack_form();
            persist_changes();
        });

        point_3d_editor_->set_on_point_selected([this](int index) {
            if (index < 0) {
                // Deselecting - persist changes before clearing selection state
                if (!frames_.empty()) {  // Guard: only persist if we have data
                    persist_changes();
                }
                if (selection_state_) {
                    selection_state_->reset();
                }
                selected_handle_ = AttackHandle::None;
                return;
            }

            // Selecting an attack handle point - guard against empty frames
            if (frames_.empty()) return;

            const int frame_index = clamp_index(selected_index_, static_cast<int>(frames_.size()));
            auto& frame = frames_[frame_index];
            if (frame.attack.vectors.empty()) return;

            int vec_idx = index / 3;
            int handle_idx = index % 3;

            if (vec_idx < static_cast<int>(frame.attack.vectors.size())) {
                set_current_attack_vector_index(vec_idx);
                selected_handle_ = static_cast<AttackHandle>(handle_idx + 1); // Start=1, Control=2, End=3

                if (selection_state_) {
                    selection_state_->attack_vector_index = vec_idx;
                    switch (selected_handle_) {
                        case AttackHandle::Start: selection_state_->target = SelectionTarget::AttackStart; break;
                        case AttackHandle::Control: selection_state_->target = SelectionTarget::AttackControl; break;
                        case AttackHandle::End: selection_state_->target = SelectionTarget::AttackEnd; break;
                        default: break;
                    }
                }
                refresh_selection_state();
            }
        });

        point_3d_editor_->set_on_position_changed([this](const SDL_FPoint& new_world_pos, float new_world_z) {
            auto* vec = current_attack_vector();
            if (!vec) return;
            SDL_FPoint snapped_world = snap_world_point_to_grid(new_world_pos, context_.snap_resolution);
            float snapped_world_z = snap_world_z_to_grid(new_world_z, context_.snap_resolution);
            SDL_Point anchor = asset_anchor_world();
            float scale = asset_local_scale();
            FramePointResolver resolver(context_.target);
            SDL_FPoint local;
            local.x = (snapped_world.x - static_cast<float>(anchor.x)) / scale;
            local.y = (static_cast<float>(anchor.y) - snapped_world.y) / scale;
            float x_percent = resolver.to_percent_xy(local.x);
            float y_percent = resolver.to_percent_xy(local.y);
            float depth_percent = resolver.to_percent_depth(snapped_world_z);
            switch (selected_handle_) {
                case AttackHandle::Start:
                    vec->start_x = x_percent;
                    vec->start_y = y_percent;
                    vec->start_z = depth_percent;
                    break;
                case AttackHandle::Control:
                    vec->control_x = x_percent;
                    vec->control_y = y_percent;
                    vec->control_z = depth_percent;
                    break;
                case AttackHandle::End:
                    vec->end_x = x_percent;
                    vec->end_y = y_percent;
                    vec->end_z = depth_percent;
                    break;
                default:
                    break;
            }
            refresh_attack_form();
            persist_changes();
        });
    }
    dirty_ = false;
    wants_close_ = false;
    selected_index_ = 0;
    selected_attack_vector_index_ = -1;
    frames_.clear();
    selected_handle_ = AttackHandle::None;

    if (context_.document) {
        auto payload_opt = context_.document->animation_payload_json(context_.animation_id);
        nlohmann::json payload = payload_opt.value_or(nlohmann::json::object());
        frames_ = parse_frames_from_payload(payload);
    }
    if (frames_.empty()) {
        frames_.push_back(MovementFrame{});
    }

    manifest_txn_.begin(context_);
    manifest_txn_.set_immediate_persist(false);
    manifest_txn_.set_apply_callback([this]() -> bool {
        if (!context_.document) {
            return false;
        }
        auto payload_opt = context_.document->animation_payload_json(context_.animation_id);
        nlohmann::json payload = payload_opt.value_or(nlohmann::json::object());
        nlohmann::json updated = build_payload_from_frames(frames_, payload);
        return context_.document->update_animation_payload(context_.animation_id, updated);
    });

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
    frame_navigator_->set_on_apply_next([this]() { apply_attack_to_next_frame(); });
    frame_navigator_->set_on_apply_animation([this]() { apply_attack_to_animation(); });
    frame_navigator_->set_on_apply_all([this]() { (void)apply_attack_to_all_animations(); });
    frame_navigator_->set_on_save_and_exit([this]() {
        if (context_.on_end) {
            context_.on_end();
        }
    });
    btn_add_remove_ = std::make_unique<DMButton>("Add Attack", &DMStyles::AccentButton(), 150, DMButton::height());
    btn_delete_ = std::make_unique<DMButton>("Delete Attack", &DMStyles::DeleteButton(), 150, DMButton::height());

    tool_panel_ = std::make_unique<FrameToolPanel>("Attack Geometry Tool Panel", "frame_editor_tool_panel_attack");
    add_remove_widget_ = std::make_unique<ButtonWidget>(btn_add_remove_.get(), [this]() {
        auto* vec = current_attack_vector();
        if (vec) {
            delete_current_attack_vector();
        } else {
            ensure_attack_vector_for_type(current_attack_type());
            persist_changes();
        }
        refresh_attack_form();
    });
    delete_widget_ = std::make_unique<ButtonWidget>(btn_delete_.get(), [this]() {
        delete_current_attack_vector();
        refresh_attack_form();
    });
    DockableCollapsible::Rows rows{
        {add_remove_widget_.get(), delete_widget_.get()},
    };
    tool_panel_->set_rows(rows);
    // Position set on first update when screen dimensions are available.

    clamp_attack_selection();
    refresh_attack_form();
    refresh_selection_state();
}

void AttackGeoFrameEditor::end() {
    frames_.clear();
    dirty_ = false;
    if (selection_state_) {
        selection_state_->reset();
        selection_state_ = nullptr;
    }
    point_3d_editor_ = nullptr;
    tool_panel_.reset();
    add_remove_widget_.reset();
    delete_widget_.reset();
    btn_add_remove_.reset();
    btn_delete_.reset();
    wants_close_ = false;
}

bool AttackGeoFrameEditor::handle_event(const SDL_Event& e) {
    // Handle Point3DEditor events first
    SDL_Rect overlay_rect{0, 0, 0, 0};
    bool overlay_valid = false;
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

    if (tool_panel_ && tool_panel_->handle_event(e)) {
        consumed = true;
    }

    if (frame_navigator_ && frame_navigator_->handle_event(e)) {
        consumed = true;
        if (selection_state_) selection_state_->reset();
        if (point_3d_editor_) point_3d_editor_->set_selected_point_index(-1);
    }

    if (e.type == SDL_EVENT_KEY_DOWN) {
        // Arrow keys navigate between points within the frame
        // Use frame navigator buttons/textbox for frame navigation
        if (e.key.key == SDLK_LEFT) {
            // Navigate to previous point
            if (point_3d_editor_) {
                int current_point = point_3d_editor_->get_selected_point_index();
                const int frame_index = clamp_index(selected_index_, static_cast<int>(frames_.size()));
                const auto& frame = frames_[frame_index];
                int total_points = static_cast<int>(frame.attack.vectors.size()) * 3;

                if (current_point > 0) {
                    int new_point = current_point - 1;
                    point_3d_editor_->set_selected_point_index(new_point);

                    // Update selection state
                    int vec_idx = new_point / 3;
                    int handle_idx = new_point % 3;
                    if (vec_idx < static_cast<int>(frame.attack.vectors.size())) {
                        set_current_attack_vector_index(vec_idx);
                        selected_handle_ = static_cast<AttackHandle>(handle_idx + 1);
                        if (selection_state_) {
                            selection_state_->attack_vector_index = vec_idx;
                            switch (selected_handle_) {
                                case AttackHandle::Start: selection_state_->target = SelectionTarget::AttackStart; break;
                                case AttackHandle::Control: selection_state_->target = SelectionTarget::AttackControl; break;
                                case AttackHandle::End: selection_state_->target = SelectionTarget::AttackEnd; break;
                                default: break;
                            }
                        }
                        refresh_selection_state();
                    }
                }
            }
            consumed = true;
        } else if (e.key.key == SDLK_RIGHT) {
            // Navigate to next point
            if (point_3d_editor_) {
                int current_point = point_3d_editor_->get_selected_point_index();
                const int frame_index = clamp_index(selected_index_, static_cast<int>(frames_.size()));
                const auto& frame = frames_[frame_index];
                int total_points = static_cast<int>(frame.attack.vectors.size()) * 3;

                if (current_point < total_points - 1) {
                    int new_point = current_point + 1;
                    point_3d_editor_->set_selected_point_index(new_point);

                    // Update selection state
                    int vec_idx = new_point / 3;
                    int handle_idx = new_point % 3;
                    if (vec_idx < static_cast<int>(frame.attack.vectors.size())) {
                        set_current_attack_vector_index(vec_idx);
                        selected_handle_ = static_cast<AttackHandle>(handle_idx + 1);
                        if (selection_state_) {
                            selection_state_->attack_vector_index = vec_idx;
                            switch (selected_handle_) {
                                case AttackHandle::Start: selection_state_->target = SelectionTarget::AttackStart; break;
                                case AttackHandle::Control: selection_state_->target = SelectionTarget::AttackControl; break;
                                case AttackHandle::End: selection_state_->target = SelectionTarget::AttackEnd; break;
                                default: break;
                            }
                        }
                        refresh_selection_state();
                    }
                }
            }
            consumed = true;
        }
    }

    if (!context_.assets || !context_.target) {
        return consumed;
    }

    SDL_Point mouse_pos = {0, 0};
    if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_UP) {
        mouse_pos = {static_cast<int>(std::lround(e.button.x)), static_cast<int>(std::lround(e.button.y))};
    } else if (e.type == SDL_EVENT_MOUSE_MOTION) {
        mouse_pos = {static_cast<int>(std::lround(e.motion.x)), static_cast<int>(std::lround(e.motion.y))};
    } else if (e.type == SDL_EVENT_MOUSE_WHEEL) {
        sdl_mouse_util::GetMouseState(&mouse_pos.x, &mouse_pos.y);
    }

    if (!ui_contains_point(mouse_pos)) {
        const int frame_index = clamp_index(selected_index_, static_cast<int>(frames_.size()));
        const auto& frame = frames_[frame_index];
        const WarpedScreenGrid& cam = context_.camera ? *context_.camera : context_.assets->getView();
        SDL_Point anchor = asset_anchor_world();
        const float scale = asset_local_scale();

        std::vector<SDL_FPoint> point_screens;
        std::vector<bool> point_selectable;
        for (const auto& vec : frame.attack.vectors) {
            auto to_screen = [&](float lx, float ly) -> SDL_FPoint {
                SDL_FPoint world{static_cast<float>(anchor.x) + lx * scale, static_cast<float>(anchor.y) - ly * scale};
                return cam.map_to_screen_f(world);
            };
            point_screens.push_back(to_screen(vec.start_x, vec.start_y));
            point_screens.push_back(to_screen(vec.control_x, vec.control_y));
            point_screens.push_back(to_screen(vec.end_x, vec.end_y));
            // All three points of the current frame's attack vectors are selectable
            point_selectable.push_back(true);
            point_selectable.push_back(true);
            point_selectable.push_back(true);
        }

        const bool pointer_in_overlay = overlay_valid && SDL_PointInRect(&mouse_pos, &overlay_rect);
        // Only consume event if point editor actually handled it and pointer not over overlay UI
        if (!pointer_in_overlay) {
            consumed = point_3d_editor_->handle_mouse_event(e, point_screens, point_selectable);
        }
    }

    return consumed;
}

void AttackGeoFrameEditor::update(const Input& input, float) {
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
    refresh_attack_form();
}

void AttackGeoFrameEditor::render_world(SDL_Renderer* renderer) const {
    if (!renderer) return;
    render_attack_geometry(renderer);
}

void AttackGeoFrameEditor::render_overlays(SDL_Renderer* renderer) const {
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

void AttackGeoFrameEditor::layout_ui(SDL_Renderer* renderer) const {
    if (!renderer) return;
    int sw = 0, sh = 0;
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

void AttackGeoFrameEditor::render_attack_geometry(SDL_Renderer* renderer) const {
    if (!renderer || frames_.empty() || !context_.assets || !context_.target) return;
    const int frame_index = clamp_index(selected_index_, static_cast<int>(frames_.size()));
    const auto& frame = frames_[frame_index];
    if (frame.attack.vectors.empty()) return;

    const WarpedScreenGrid& cam = context_.camera ? *context_.camera : context_.assets->getView();
    SDL_Point anchor = asset_anchor_world();
    const float scale = asset_local_scale();
    if (scale <= 0.0001f) return;

    FramePointResolver resolver(context_.target);
    auto to_screen = [&](float lx_percent, float ly_percent) -> SDL_FPoint {
        // Convert percent values to local world coordinates
        const float lx = resolver.to_world_xy(lx_percent);
        const float ly = resolver.to_world_xy(ly_percent);
        SDL_FPoint world{static_cast<float>(anchor.x) + lx * scale, static_cast<float>(anchor.y) - ly * scale};
        return cam.map_to_screen_f(world);
    };

    const int selected_idx = current_attack_vector_index();
    std::size_t vec_idx = 0;
    for (const auto& vec : frame.attack.vectors) {
        const bool selected = (static_cast<int>(vec_idx) == selected_idx && selected_idx >= 0);
        SDL_FPoint start_screen = to_screen(vec.start_x, vec.start_y);
        SDL_FPoint control_screen = to_screen(vec.control_x, vec.control_y);
        SDL_FPoint end_screen = to_screen(vec.end_x, vec.end_y);

        SDL_Color line_color = selected ? DMStyles::AccentButton().bg : DMStyles::HeaderButton().bg;
        SDL_SetRenderDrawColor(renderer, line_color.r, line_color.g, line_color.b, 220);
        constexpr int segments = 16;
        SDL_FPoint prev = start_screen;
        for (int i = 1; i <= segments; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(segments);
            const float u = 1.0f - t;
            SDL_FPoint p{
                u * u * start_screen.x + 2.0f * u * t * control_screen.x + t * t * end_screen.x,
                u * u * start_screen.y + 2.0f * u * t * control_screen.y + t * t * end_screen.y};
            SDL_RenderLine(renderer, prev.x, prev.y, p.x, p.y);
            prev = p;
        }

        if (selected) {
            SDL_SetRenderDrawColor(renderer, 180, 180, 180, 180);
            SDL_RenderLine(renderer, start_screen.x, start_screen.y, control_screen.x, control_screen.y);
            SDL_RenderLine(renderer, control_screen.x, control_screen.y, end_screen.x, end_screen.y);
        }

        // Determine which point is currently selected
        bool start_selected = selected && selected_handle_ == AttackHandle::Start;
        bool control_selected = selected && selected_handle_ == AttackHandle::Control;
        bool end_selected = selected && selected_handle_ == AttackHandle::End;

        // Render all three points as 3D axis points
        point_3d_editor_->render_axis_point(renderer, start_screen,
                                          selection_state_ ? selection_state_->axis : AdjustmentAxis::X,
                                          start_selected);
        point_3d_editor_->render_axis_point(renderer, control_screen,
                                          selection_state_ ? selection_state_->axis : AdjustmentAxis::X,
                                          control_selected);
        point_3d_editor_->render_axis_point(renderer, end_screen,
                                          selection_state_ ? selection_state_->axis : AdjustmentAxis::X,
                                          end_selected);
        ++vec_idx;
    }
}

void AttackGeoFrameEditor::select_frame(int index) {
    selected_index_ = clamp_index(index, static_cast<int>(frames_.size()));
    clamp_attack_selection();
    refresh_attack_form();

    // Deselect point when changing frames
    if (point_3d_editor_) {
        point_3d_editor_->set_selected_point_index(-1);
    }
    if (selection_state_) {
        selection_state_->reset();
    }
    selected_handle_ = AttackHandle::None;

    if (frame_navigator_) {
        frame_navigator_->set_current_frame(selected_index_);
    }
}

void AttackGeoFrameEditor::clamp_attack_selection() {
    if (frames_.empty()) {
        selected_attack_vector_index_ = -1;
        return;
    }
    const int frame_index = clamp_index(selected_index_, static_cast<int>(frames_.size()));
    if (frame_index >= static_cast<int>(frames_.size())) {
        selected_attack_vector_index_ = -1;
        return;
    }
    const int count = static_cast<int>(frames_[frame_index].attack.vectors.size());
    if (count == 0) {
        selected_attack_vector_index_ = -1;
        return;
    }
    selected_attack_vector_index_ = std::clamp(selected_attack_vector_index_, 0, count - 1);
}

void AttackGeoFrameEditor::refresh_attack_form() const {
    const_cast<AttackGeoFrameEditor*>(this)->clamp_attack_selection();
    const auto* vec = current_attack_vector();
    if (vec) {
        if (btn_add_remove_) btn_add_remove_->set_text("Delete Attack");
        if (btn_delete_) btn_delete_->set_text("Delete Attack");
    } else {
        if (btn_add_remove_) btn_add_remove_->set_text("Add Attack");
        if (btn_delete_) btn_delete_->set_text("Delete Attack");
    }
}



void AttackGeoFrameEditor::persist_changes() {
    // Guard: only persist if we have data
    if (frames_.empty()) {
        return;
    }
    apply_live_changes();
    clamp_attack_selection();
    refresh_attack_form();
    refresh_selection_state();
}

void AttackGeoFrameEditor::persist_pending_changes() {
    if (!manifest_txn_.active() || !dirty_) {
        return;
    }
    if (manifest_txn_.commit(true)) {
        dirty_ = false;
        invalidate_preview();
    }
}

float AttackGeoFrameEditor::base_world_z() const {
    return context_.target ? context_.target->world_z_offset() : 0.0f;
}

void AttackGeoFrameEditor::apply_attack_to_all_frames() {
    if (frames_.empty()) return;
    const int idx = clamp_index(selected_index_, static_cast<int>(frames_.size()));
    animation_update::FrameAttackGeometry src = frames_[idx].attack;
    for (auto& f : frames_) {
        f.attack = src;
    }
    refresh_attack_form();
    persist_changes();
    persist_pending_changes();
    invalidate_preview();
}

void AttackGeoFrameEditor::apply_attack_to_next_frame() {
    if (frames_.empty()) return;
    const int count = static_cast<int>(frames_.size());
    const int idx = clamp_index(selected_index_, count);
    const int target = (idx + 1) % count;
    frames_[target].attack = frames_[idx].attack;
    persist_changes();
    persist_pending_changes();
    if (context_.preview && !context_.animation_id.empty()) {
        context_.preview->invalidate(context_.animation_id);
    }
}

void AttackGeoFrameEditor::apply_attack_to_animation() {
    apply_attack_to_all_frames();
}

bool AttackGeoFrameEditor::apply_attack_to_all_animations() {
    if (!context_.document || frames_.empty()) return false;
    apply_attack_to_animation();

    const int idx = clamp_index(selected_index_, static_cast<int>(frames_.size()));
    const animation_update::FrameAttackGeometry src = frames_[idx].attack;
    const auto ids = context_.document->animation_ids();
    for (const auto& id : ids) {
        auto payload_opt = context_.document->animation_payload_json(id);
        nlohmann::json payload = payload_opt.value_or(nlohmann::json::object());
        auto frames = parse_frames_from_payload(payload);
        if (frames.empty()) frames.push_back(MovementFrame{});
        for (auto& f : frames) {
            f.attack = src;
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

std::string AttackGeoFrameEditor::current_attack_type() const {
    return "";  // No type system
}

int AttackGeoFrameEditor::current_attack_vector_index() const {
    return selected_attack_vector_index_;
}

void AttackGeoFrameEditor::set_current_attack_vector_index(int index) {
    selected_attack_vector_index_ = index;
}

animation_update::FrameAttackGeometry::Vector* AttackGeoFrameEditor::current_attack_vector() {
    clamp_attack_selection();
    const int vector_index = current_attack_vector_index();
    if (frames_.empty() || vector_index < 0) return nullptr;
    const int frame_index = clamp_index(selected_index_, static_cast<int>(frames_.size()));
    auto& frame = frames_[frame_index];
    if (vector_index >= static_cast<int>(frame.attack.vectors.size())) return nullptr;
    return &frame.attack.vectors[static_cast<std::size_t>(vector_index)];
}

const animation_update::FrameAttackGeometry::Vector* AttackGeoFrameEditor::current_attack_vector() const {
    return const_cast<AttackGeoFrameEditor*>(this)->current_attack_vector();
}

animation_update::FrameAttackGeometry::Vector* AttackGeoFrameEditor::ensure_attack_vector_for_type(const std::string& /*type*/) {
    if (frames_.empty()) return nullptr;
    const int frame_index = clamp_index(selected_index_, static_cast<int>(frames_.size()));
    auto& frame = frames_[frame_index];
    animation_update::FrameAttackGeometry::Vector vec;
    vec.type = "";  // No type
    frame.attack.vectors.push_back(vec);
    set_current_attack_vector_index(static_cast<int>(frame.attack.vectors.size()) - 1);
    return &frame.attack.vectors.back();
}

void AttackGeoFrameEditor::delete_current_attack_vector() {
    if (frames_.empty()) return;
    const int frame_index = clamp_index(selected_index_, static_cast<int>(frames_.size()));
    auto& frame = frames_[frame_index];
    const int index = current_attack_vector_index();
    if (index >= 0 && index < static_cast<int>(frame.attack.vectors.size())) {
        frame.attack.vectors.erase(frame.attack.vectors.begin() + index);
    }
    clamp_attack_selection();
    persist_changes();
}

void AttackGeoFrameEditor::apply_live_changes() {
    dirty_ = true;
}

void AttackGeoFrameEditor::invalidate_preview() const {
    if (context_.preview && !context_.animation_id.empty()) {
        context_.preview->invalidate(context_.animation_id);
    }
}


SDL_Point AttackGeoFrameEditor::asset_anchor_world() const {
    if (!context_.target) {
        return SDL_Point{0, 0};
    }
    return animation_update::detail::bottom_middle_for(*context_.target, context_.target->world_point());
}

float AttackGeoFrameEditor::asset_local_scale() const {
    if (!context_.assets || !context_.target) {
        return 1.0f;
    }
    const WarpedScreenGrid& cam = context_.camera ? *context_.camera : context_.assets->getView();
    float perspective_scale = 1.0f;
    if (const auto* gp = cam.grid_point_for_asset(context_.target)) {
        perspective_scale = std::max(0.0001f, gp->perspective_scale);
    }
    float remainder = context_.target->current_remaining_scale_adjustment;
    if (!std::isfinite(remainder) || remainder <= 0.0f) {
        remainder = 1.0f;
    }
    float scale = remainder / std::max(0.0001f, perspective_scale);
    if (!std::isfinite(scale) || scale <= 0.0f) {
        scale = 1.0f;
    }
    return scale;
}

bool AttackGeoFrameEditor::screen_to_local(SDL_Point screen, SDL_FPoint& out_local) const {
    if (!context_.assets || !context_.target) return false;
    const WarpedScreenGrid& cam = context_.camera ? *context_.camera : context_.assets->getView();
    SDL_FPoint world = cam.screen_to_map(screen);
    SDL_Point anchor = asset_anchor_world();
    const float scale = asset_local_scale();
    if (scale <= 0.0001f) return false;
    out_local.x = (world.x - static_cast<float>(anchor.x)) / scale;
    out_local.y = (static_cast<float>(anchor.y) - world.y) / scale;
    return std::isfinite(out_local.x) && std::isfinite(out_local.y);
}

SDL_FPoint AttackGeoFrameEditor::screen_to_world_point(const SDL_Point& screen) const {
    SDL_FPoint local;
    if (!screen_to_local(screen, local)) return SDL_FPoint{0.0f, 0.0f};
    SDL_Point anchor = asset_anchor_world();
    float scale = asset_local_scale();
    return SDL_FPoint{static_cast<float>(anchor.x) + local.x * scale, static_cast<float>(anchor.y) - local.y * scale};
}

void AttackGeoFrameEditor::refresh_selection_state() {
    if (!selection_state_ || !context_.assets || !context_.target) {
        return;
    }
    if (!selection_state_->has_target()) {
        return;
    }
    FramePointResolver resolver(context_.target);
    // Update parent height for Z percent display (in case scale changed)
    if (point_3d_editor_) {
        point_3d_editor_->set_parent_height(resolver.parent_height_px());
    }
    const auto* vec = current_attack_vector();
    if (!vec) {
        selection_state_->target = SelectionTarget::None;
        selection_state_->attack_vector_index = -1;
        return;
    }
    selection_state_->attack_vector_index = current_attack_vector_index();
    float local_x = vec->start_x;
    float local_y = vec->start_y;
    switch (selection_state_->target) {
        case SelectionTarget::AttackControl:
            local_x = vec->control_x;
            local_y = vec->control_y;
            break;
        case SelectionTarget::AttackEnd:
            local_x = vec->end_x;
            local_y = vec->end_y;
            break;
        case SelectionTarget::AttackStart:
        default:
            break;
    }
    float depth_percent = vec ? vec->start_z : 0.0f;
    if (vec) {
        switch (selection_state_->target) {
            case SelectionTarget::AttackControl: depth_percent = vec->control_z; break;
            case SelectionTarget::AttackEnd: depth_percent = vec->end_z; break;
            case SelectionTarget::AttackStart:
            default: depth_percent = vec->start_z; break;
        }
    }
    SDL_Point anchor = asset_anchor_world();
    const float base_z = resolver.base_world_depth();
    // Convert percent values back to world coordinates
    const float local_x_world = resolver.to_world_xy(local_x) * asset_local_scale();
    const float local_y_world = resolver.to_world_xy(local_y) * asset_local_scale();
    SDL_FPoint world{
        static_cast<float>(anchor.x) + local_x_world,
        static_cast<float>(anchor.y) - local_y_world
    };
    const WarpedScreenGrid& cam = context_.camera ? *context_.camera : context_.assets->getView();
    SDL_FPoint screen = cam.map_to_screen_f(world);
    selection_state_->world_pos = world;
    selection_state_->world_z = resolver.to_world_depth(depth_percent);
    selection_state_->screen_pos = round_point(screen);
    selection_state_->set_anchor_world(anchor, base_z);
}

bool AttackGeoFrameEditor::ui_contains_point(const SDL_Point& p) const {
    if (SDL_PointInRect(&p, &nav_rect_)) return true;
    return tool_panel_ && tool_panel_->contains_point(p);
}

}  // namespace devmode::frame_editors


