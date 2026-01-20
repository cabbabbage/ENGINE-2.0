#include "AttackGeoFrameEditor.hpp"

#include <SDL.h>

#include <algorithm>
#include <cmath>
#include <utility>

#include "animation_update/animation_update.hpp"
#include "dev_mode/asset_sections/animation_editor_window/AnimationDocument.hpp"
#include "dev_mode/dm_styles.hpp"
#include "dev_mode/draw_utils.hpp"
#include "dev_mode/dev_mode_utils.hpp"
#include "dev_mode/widgets.hpp"

#include "nlohmann/json.hpp"
#include "render/warped_screen_grid.hpp"

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
        point_3d_editor_->set_on_coordinates_changed([this]() {
            persist_changes();
        });

        point_3d_editor_->set_on_point_selected([this](int index) {
            if (index < 0) {
                // Deselecting - clear selection state without changing frame
                if (selection_state_) {
                    selection_state_->reset();
                }
                selected_handle_ = AttackHandle::None;
                return;
            }

            // Selecting an attack handle point
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
            SDL_Point anchor = asset_anchor_world();
            float scale = asset_local_scale();
            SDL_FPoint local;
            local.x = (new_world_pos.x - static_cast<float>(anchor.x)) / scale;
            local.y = (static_cast<float>(anchor.y) - new_world_pos.y) / scale;
            float local_z = (new_world_z - base_world_z()) / scale;
            switch (selected_handle_) {
                case AttackHandle::Start:
                    vec->start_x = local.x;
                    vec->start_y = local.y;
                    vec->start_z = local_z;
                    break;
                case AttackHandle::Control:
                    vec->control_x = local.x;
                    vec->control_y = local.y;
                    vec->control_z = local_z;
                    break;
                case AttackHandle::End:
                    vec->end_x = local.x;
                    vec->end_y = local.y;
                    vec->end_z = local_z;
                    break;
                default:
                    break;
            }
            refresh_attack_form();
            persist_changes();
        });
    }
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
    manifest_txn_.set_immediate_persist(true);
    manifest_txn_.set_apply_callback([this]() -> bool {
        if (!context_.document) {
            return false;
        }
        auto payload_opt = context_.document->animation_payload_json(context_.animation_id);
        nlohmann::json payload = payload_opt.value_or(nlohmann::json::object());
        nlohmann::json updated = build_payload_from_frames(frames_, payload);
        return context_.document->save_animation_payload_immediately(context_.animation_id, updated);
    });

    btn_back_ = std::make_unique<DMButton>("Back", &DMStyles::HeaderButton(), 80, DMButton::height());
    frame_navigator_ = std::make_unique<FrameNavigator>();
    frame_navigator_->set_frame_count(static_cast<int>(frames_.size()));
    frame_navigator_->set_current_frame(selected_index_);
    frame_navigator_->set_on_frame_changed([this](int frame) {
        select_frame(frame);
    });
    btn_add_remove_ = std::make_unique<DMButton>("Add Attack", &DMStyles::AccentButton(), 150, DMButton::height());
    btn_delete_ = std::make_unique<DMButton>("Delete Attack", &DMStyles::DeleteButton(), 150, DMButton::height());
    btn_copy_next_ = std::make_unique<DMButton>("Copy To Next", &DMStyles::HeaderButton(), 150, DMButton::height());
    btn_apply_all_ = std::make_unique<DMButton>("Apply To All Frames", &DMStyles::HeaderButton(), 180, DMButton::height());

    clamp_attack_selection();
    refresh_attack_form();
    refresh_selection_state();
}

void AttackGeoFrameEditor::end() {
    if (manifest_txn_.active()) {
        manifest_txn_.commit();
    }
    frames_.clear();
    if (selection_state_) {
        selection_state_->reset();
        selection_state_ = nullptr;
    }
    point_3d_editor_ = nullptr;
    btn_back_.reset();
    btn_add_remove_.reset();
    btn_delete_.reset();
    btn_copy_next_.reset();
    btn_apply_all_.reset();
    wants_close_ = false;
}

bool AttackGeoFrameEditor::handle_event(const SDL_Event& e) {
    // Handle Point3DEditor events first
    if (point_3d_editor_) {
        int sw = 0, sh = 0;
        SDL_GetRendererOutputSize(nullptr, &sw, &sh);
        int height = point_3d_editor_->get_overlay_height();
        SDL_Rect bottom_container{0, sh - height, sw, height};
        if (point_3d_editor_->handle_event(e, bottom_container)) {
            return true;
        }
    }

    bool consumed = false;

    if (frame_navigator_ && frame_navigator_->handle_event(e)) {
        consumed = true;
    }
    if (btn_back_ && btn_back_->handle_event(e)) {
        wants_close_ = true;
        consumed = true;
    }

    if (btn_add_remove_ && btn_add_remove_->handle_event(e)) {
        auto* vec = current_attack_vector();
        if (vec) {
            delete_current_attack_vector();
        } else {
            ensure_attack_vector_for_type(current_attack_type());
        }
        refresh_attack_form();
        consumed = true;
    }
    if (btn_delete_ && btn_delete_->handle_event(e)) {
        delete_current_attack_vector();
        refresh_attack_form();
        consumed = true;
    }
    if (btn_copy_next_ && btn_copy_next_->handle_event(e)) {
        copy_attack_vector_to_next_frame();
        consumed = true;
    }
    if (btn_apply_all_ && btn_apply_all_->handle_event(e)) {
        apply_attack_to_all_frames();
        consumed = true;
    }

    if (e.type == SDL_KEYDOWN) {
        // Arrow keys navigate between points within the frame
        // Use frame navigator buttons/textbox for frame navigation
        if (e.key.keysym.sym == SDLK_LEFT) {
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
        } else if (e.key.keysym.sym == SDLK_RIGHT) {
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
    if (e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEBUTTONUP) {
        mouse_pos = {e.button.x, e.button.y};
    } else if (e.type == SDL_MOUSEMOTION) {
        mouse_pos = {e.motion.x, e.motion.y};
    } else if (e.type == SDL_MOUSEWHEEL) {
        SDL_GetMouseState(&mouse_pos.x, &mouse_pos.y);
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

        // Only consume event if point editor actually handled it
        consumed = point_3d_editor_->handle_mouse_event(e, point_screens, point_selectable, [this](const SDL_Point& p) {
                return screen_to_world_point(p);
            });
    }

    return consumed;
}

void AttackGeoFrameEditor::update(const Input&, float) {
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
    if (btn_back_) btn_back_->render(renderer);
    if (frame_navigator_) frame_navigator_->render(renderer);

    if (btn_add_remove_) btn_add_remove_->render(renderer);
    if (btn_delete_) btn_delete_->render(renderer);
    if (btn_copy_next_) btn_copy_next_->render(renderer);
    if (btn_apply_all_) btn_apply_all_->render(renderer);

    // Render Point3DEditor overlays at the bottom
    if (point_3d_editor_) {
        int sw = 0, sh = 0;
        SDL_GetRendererOutputSize(renderer, &sw, &sh);
        int height = point_3d_editor_->get_overlay_height();
        SDL_Rect bottom_container{0, sh - height, sw, height};
        point_3d_editor_->render_overlays(renderer, bottom_container);
    }
}

void AttackGeoFrameEditor::layout_ui(SDL_Renderer* renderer) const {
    if (!renderer) return;
    int sw = 0, sh = 0;
    SDL_GetRendererOutputSize(renderer, &sw, &sh);
    const int padding = DMSpacing::small_gap();
    const int width = 320;
    const int x = padding;
    const int y = padding;
    ui_rect_ = SDL_Rect{x, y, width, 0};
    int inner_w = width - padding * 2;
    int cursor_y = y + padding;

    auto place_row = [&](int h) -> SDL_Rect {
        SDL_Rect r{x + padding, cursor_y, inner_w, h};
        cursor_y += h + DMSpacing::small_gap();
        return r;
    };

    if (btn_back_) {
        btn_back_->set_rect(place_row(DMButton::height()));
    }

    if (frame_navigator_) {
        SDL_Rect nav_rect{x + padding, cursor_y, inner_w, frame_navigator_->get_preferred_rect().h};
        frame_navigator_->set_rect(nav_rect);
        cursor_y += nav_rect.h + DMSpacing::small_gap();
    }



    if (btn_add_remove_ || btn_delete_ || btn_copy_next_) {
        int button_count = 0;
        if (btn_add_remove_) ++button_count;
        if (btn_delete_) ++button_count;
        if (btn_copy_next_) ++button_count;
        const int total_gap = DMSpacing::small_gap() * std::max(0, button_count - 1);
        const int button_w = (inner_w - total_gap) / std::max(1, button_count);
        SDL_Rect row = place_row(DMButton::height());
        int offset_x = row.x;
        auto place_btn = [&](DMButton* btn) {
            if (!btn) return;
            btn->set_rect(SDL_Rect{offset_x, row.y, button_w, row.h});
            offset_x += button_w + DMSpacing::small_gap();
        };
        place_btn(btn_add_remove_.get());
        place_btn(btn_delete_.get());
        place_btn(btn_copy_next_.get());
    }


    if (btn_apply_all_) {
        btn_apply_all_->set_rect(place_row(DMButton::height()));
    }
    ui_rect_.h = cursor_y - y;
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

    auto to_screen = [&](float lx, float ly) -> SDL_FPoint {
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
            SDL_RenderDrawLineF(renderer, prev.x, prev.y, p.x, p.y);
            prev = p;
        }

        if (selected) {
            SDL_SetRenderDrawColor(renderer, 180, 180, 180, 180);
            SDL_RenderDrawLineF(renderer, start_screen.x, start_screen.y, control_screen.x, control_screen.y);
            SDL_RenderDrawLineF(renderer, control_screen.x, control_screen.y, end_screen.x, end_screen.y);
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
    if (manifest_txn_.active()) {
        manifest_txn_.commit();
    }
    clamp_attack_selection();
    refresh_attack_form();
    refresh_selection_state();
}

float AttackGeoFrameEditor::base_world_z() const {
    return context_.target ? context_.target->world_z_offset() : 0.0f;
}

void AttackGeoFrameEditor::apply_attack_to_all_frames() {
    const auto* source = current_attack_vector();
    if (!source) return;
    for (auto& f : frames_) {
        f.attack.vectors.clear();
        f.attack.vectors.push_back(*source);
    }
    refresh_attack_form();
    persist_changes();
}

void AttackGeoFrameEditor::copy_attack_vector_to_next_frame() {
    if (frames_.empty()) return;
    const int next_index = selected_index_ + 1;
    if (next_index >= static_cast<int>(frames_.size())) {
        return;
    }
    const auto* source = current_attack_vector();
    if (!source) return;
    auto& dest_vecs = frames_[next_index].attack.vectors;
    dest_vecs.clear();
    dest_vecs.push_back(*source);
    set_current_attack_vector_index(0);
    persist_changes();
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


SDL_Point AttackGeoFrameEditor::asset_anchor_world() const {
    if (!context_.target) {
        return SDL_Point{0, 0};
    }
    return animation_update::detail::bottom_middle_for(*context_.target, context_.target->pos);
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
    SDL_Point anchor = asset_anchor_world();
    SDL_FPoint world{
        static_cast<float>(anchor.x) + local_x * asset_local_scale(),
        static_cast<float>(anchor.y) - local_y * asset_local_scale()
    };
    const WarpedScreenGrid& cam = context_.camera ? *context_.camera : context_.assets->getView();
    SDL_FPoint screen = cam.map_to_screen_f(world);
    selection_state_->world_pos = world;
    selection_state_->screen_pos = round_point(screen);
}

bool AttackGeoFrameEditor::ui_contains_point(const SDL_Point& p) const {
    return SDL_PointInRect(&p, &ui_rect_) == SDL_TRUE;
}

}  // namespace devmode::frame_editors
