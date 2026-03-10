#include "HitGeoFrameEditor.hpp"
#include "utils/sdl_mouse_utils.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cmath>

#include "animation/animation_update.hpp"
#include "devtools/asset_editor/animation_editor_window/AnimationDocument.hpp"
#include "devtools/dm_styles.hpp"
#include "devtools/draw_utils.hpp"
#include "devtools/dev_mode_utils.hpp"
#include "devtools/widgets.hpp"
#include "devtools/frame_editors/shared/SnapUtils.hpp"
#include "utils/FramePointResolver.hpp"
#include "core/axis_convention.hpp"
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

constexpr float kPi = 3.14159265358979323846f;

float dist_sq(const SDL_FPoint& a, const SDL_FPoint& b) {
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    return dx * dx + dy * dy;
}

}  // namespace

void HitGeoFrameEditor::begin(const FrameEditorContext& context) {
    context_ = context;
    selection_state_ = context.selection_state;
    point_3d_editor_ = std::make_unique<Point3DEditor>(selection_state_);
    if (selection_state_) {
        selection_state_->reset();
    }
    if (point_3d_editor_) {
        point_3d_editor_->reset_axis(AdjustmentAxis::X);
        point_3d_editor_->set_grid_resolution(context_.snap_resolution);
        // Hit geometry uses Z as percentage of parent height
        point_3d_editor_->set_z_display_mode(CoordinateDisplayMode::Percentage);
        FramePointResolver resolver(context_.target);
        point_3d_editor_->set_parent_height(resolver.parent_height_px());

        point_3d_editor_->set_on_point_selected([this](int index) {
            if (index < 0) {
                // Deselecting - persist changes before clearing selection state
                if (!frames_.empty()) {  // Guard: only persist if we have data
                    persist_changes();
                }
                if (selection_state_) {
                    selection_state_->reset();
                }
            } else {
                // Only handle selection if it's the current frame's hitbox
                if (index == selected_index_ && !frames_.empty()) {
                    if (selection_state_) {
                        selection_state_->target = SelectionTarget::HitboxCenter;
                    }
                    if (point_3d_editor_) {
                        point_3d_editor_->set_selected_point_index(index);
                    }
                    refresh_selection_state();
                }
            }
        });

        point_3d_editor_->set_on_position_changed([this](const SDL_FPoint& new_world_pos, float new_world_z) {
            // Guard: ensure frames exist
            if (frames_.empty() || selected_index_ < 0 || selected_index_ >= static_cast<int>(frames_.size())) {
                return;
            }

            auto* box = current_hit_box();
            if (!box) return;

            SDL_FPoint snapped_world = snap_world_point_to_grid(new_world_pos, context_.snap_resolution);
            float snapped_world_z = snap_world_z_to_grid(new_world_z, context_.snap_resolution);
            SDL_Point anchor = asset_anchor_world();
            float scale = asset_local_scale();
            FramePointResolver resolver(context_.target);
            float center_x_local = (snapped_world.x - static_cast<float>(anchor.x)) / scale;
            float center_y_local = (static_cast<float>(anchor.y) - snapped_world.y) / scale;
            box->center_x = resolver.to_percent_xy(center_x_local);
            box->center_y = resolver.to_percent_xy(center_y_local);
            box->center_z = resolver.to_percent_depth(snapped_world_z);

            persist_changes();
            refresh_selection_state();
        });

        point_3d_editor_->set_on_coordinates_changed([this]() {
            // Guard: ensure frames exist
            if (frames_.empty() || selected_index_ < 0 || selected_index_ >= static_cast<int>(frames_.size())) {
                return;
            }

            auto* box = current_hit_box();
            if (!box || !selection_state_) return;

            SDL_FPoint snapped_world = snap_world_point_to_grid(selection_state_->world_pos, context_.snap_resolution);
            float snapped_world_z = snap_world_z_to_grid(selection_state_->world_z, context_.snap_resolution);
            selection_state_->world_pos = snapped_world;
            selection_state_->world_z = snapped_world_z;
            SDL_Point anchor = asset_anchor_world();
            float scale = asset_local_scale();

            FramePointResolver resolver(context_.target);
            float center_x_local = (snapped_world.x - static_cast<float>(anchor.x)) / scale;
            float center_y_local = (static_cast<float>(anchor.y) - snapped_world.y) / scale;
            box->center_x = resolver.to_percent_xy(center_x_local);
            box->center_y = resolver.to_percent_xy(center_y_local);
            box->center_z = resolver.to_percent_depth(snapped_world_z);

            persist_changes();
        });
    }
    dirty_ = false;
    wants_close_ = false;
    selected_index_ = 0;
    selected_hitbox_type_index_ = 1;
    frames_.clear();

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

    std::vector<std::string> hitbox_labels;
    hitbox_labels.reserve(kDamageTypeNames.size());
    for (const char* type : kDamageTypeNames) {
        hitbox_labels.emplace_back(type);
    }
    dd_hitbox_type_ = std::make_unique<DMDropdown>(
        "Hitbox Type", hitbox_labels,
        std::clamp(selected_hitbox_type_index_, 0, static_cast<int>(hitbox_labels.size()) - 1));
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
    frame_navigator_->set_on_apply_next([this]() { apply_hit_to_next_frame(); });
    frame_navigator_->set_on_apply_animation([this]() { apply_hit_to_animation(); });
    frame_navigator_->set_on_apply_all([this]() { (void)apply_hit_to_all_animations(); });
    frame_navigator_->set_on_save_and_exit([this]() {
        if (context_.on_end) {
            context_.on_end();
        }
    });
    btn_add_remove_ = std::make_unique<DMButton>("Add Hit Box", &DMStyles::AccentButton(), 150, DMButton::height());

    tool_panel_ = std::make_unique<FrameToolPanel>("Hit Geometry Tool Panel", "frame_editor_tool_panel_hit");
    hitbox_type_widget_ = std::make_unique<DropdownWidget>(dd_hitbox_type_.get());
    add_remove_widget_ = std::make_unique<ButtonWidget>(btn_add_remove_.get(), [this]() {
        auto* box = current_hit_box();
        const std::string type = current_hitbox_type();
        if (box) {
            delete_hit_box_for_type(type);
        } else {
            ensure_hit_box_for_type(type);
            persist_changes();
        }
        refresh_hitbox_form();
    });
    DockableCollapsible::Rows rows{
        {hitbox_type_widget_.get()},
        {add_remove_widget_.get()},
    };
    tool_panel_->set_rows(rows);
    // Position set on first update when screen dimensions are available.

    refresh_hitbox_form();
    refresh_selection_state();
}

void HitGeoFrameEditor::end() {
    frames_.clear();
    dirty_ = false;
    if (selection_state_) {
        selection_state_->reset();
        selection_state_ = nullptr;
    }
    point_3d_editor_ = nullptr;
    tool_panel_.reset();
    hitbox_type_widget_.reset();
    add_remove_widget_.reset();
    dd_hitbox_type_.reset();
    frame_navigator_.reset();
    btn_add_remove_.reset();
    wants_close_ = false;
}

bool HitGeoFrameEditor::handle_event(const SDL_Event& e) {
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

    if (tool_panel_) {
        const int prev_selection = dd_hitbox_type_ ? dd_hitbox_type_->selected() : selected_hitbox_type_index_;
        const bool prev_has_box = current_hit_box() != nullptr;
        if (tool_panel_->handle_event(e)) {
            consumed = true;
        }
        if (dd_hitbox_type_) {
            selected_hitbox_type_index_ = std::clamp(dd_hitbox_type_->selected(),
                                                     0,
                                                     static_cast<int>(kDamageTypeNames.size()) - 1);
        }
        if (selected_hitbox_type_index_ != prev_selection || prev_has_box != (current_hit_box() != nullptr)) {
            refresh_hitbox_form();
            consumed = true;
        }
    }

    if (frame_navigator_ && frame_navigator_->handle_event(e)) {
        consumed = true;
        if (selection_state_) selection_state_->reset();
        if (point_3d_editor_) point_3d_editor_->set_selected_point_index(-1);
    }

    // No arrow key navigation - use frame navigator buttons only

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

    const bool pointer_in_overlay = overlay_valid && SDL_PointInRect(&mouse_pos, &overlay_rect);
    if (!ui_contains_point(mouse_pos) && !pointer_in_overlay) {
        std::vector<SDL_FPoint> point_screens;
        std::vector<bool> point_selectable;
        const std::string type = current_hitbox_type();
        SDL_Point anchor = asset_anchor_world();
        const float scale = asset_local_scale();
        const WarpedScreenGrid& cam = context_.camera ? *context_.camera : context_.assets->getView();

        for (std::size_t i = 0; i < frames_.size(); ++i) {
            const auto& frame = frames_[i];
            // Find hitbox of current type in this frame
            const animation_update::FrameHitGeometry::HitBox* box = nullptr;
            for (const auto& b : frame.hit.boxes) {
                if (b.type == type) {
                    box = &b;
                    break;
                }
            }
            if (box) {
                SDL_FPoint world{static_cast<float>(anchor.x) + box->center_x * scale,
                                static_cast<float>(anchor.y) - box->center_y * scale};
                point_screens.push_back(cam.map_to_screen_f(world));
                // Only current frame's hitbox is selectable
                point_selectable.push_back(static_cast<int>(i) == selected_index_);
            } else {
                // No hitbox of this type in this frame - add a dummy off-screen point
                point_screens.push_back(SDL_FPoint{-10000.0f, -10000.0f});
                point_selectable.push_back(false);
            }
        }

        // Only consume event if point editor actually handled it
        consumed = point_3d_editor_->handle_mouse_event(e, point_screens, point_selectable);
    }

    return consumed;
}

void HitGeoFrameEditor::update(const Input& input, float) {
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
    refresh_hitbox_form();
}

void HitGeoFrameEditor::render_world(SDL_Renderer* renderer) const {
    if (!renderer) return;
    render_hit_geometry(renderer);
}

void HitGeoFrameEditor::render_overlays(SDL_Renderer* renderer) const {
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

void HitGeoFrameEditor::layout_ui(SDL_Renderer* renderer) const {
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

void HitGeoFrameEditor::select_frame(int index) {
    selected_index_ = clamp_index(index, static_cast<int>(frames_.size()));

    // Deselect point when changing frames
    if (point_3d_editor_) {
        point_3d_editor_->set_selected_point_index(-1);
    }
    if (selection_state_) {
        selection_state_->reset();
    }

    refresh_hitbox_form();
    if (frame_navigator_) {
        frame_navigator_->set_current_frame(selected_index_);
    }
}

void HitGeoFrameEditor::refresh_hitbox_form() const {
    const auto* box = current_hit_box();
    if (box) {
        if (btn_add_remove_) btn_add_remove_->set_text("Delete Hit Box");
    } else {
        if (btn_add_remove_) btn_add_remove_->set_text("Add Hit Box");
    }
}

void HitGeoFrameEditor::persist_changes() {
    // Guard: only persist if we have data
    if (frames_.empty()) {
        return;
    }
    apply_live_changes();
    refresh_hitbox_form();
    refresh_selection_state();
}

void HitGeoFrameEditor::persist_pending_changes() {
    if (!manifest_txn_.active() || !dirty_) {
        return;
    }
    if (manifest_txn_.commit(true)) {
        dirty_ = false;
        invalidate_preview();
    }
}

float HitGeoFrameEditor::base_world_z() const {
    return context_.target ? context_.target->world_z_offset() : 0.0f;
}

void HitGeoFrameEditor::apply_hit_to_animation() {
    if (frames_.empty()) return;
    const std::string type = current_hitbox_type();
    const auto* source = current_hit_box();
    for (auto& f : frames_) {
        auto& boxes = f.hit.boxes;
        boxes.erase(std::remove_if(boxes.begin(), boxes.end(),
                                   [&](const auto& b) { return b.type == type; }),
                    boxes.end());
        if (source) {
            boxes.push_back(*source);
        }
    }
    refresh_hitbox_form();
    persist_changes();
    persist_pending_changes();
    invalidate_preview();
}

void HitGeoFrameEditor::apply_live_changes() {
    dirty_ = true;
}

void HitGeoFrameEditor::invalidate_preview() const {
    if (context_.preview && !context_.animation_id.empty()) {
        context_.preview->invalidate(context_.animation_id);
    }
}

void HitGeoFrameEditor::apply_hit_to_next_frame() {
    if (frames_.empty()) return;
    const int count = static_cast<int>(frames_.size());
    const int idx = clamp_index(selected_index_, count);
    const int target = (idx + 1) % count;
    const std::string type = current_hitbox_type();
    const auto* source = current_hit_box();
    auto& dest_boxes = frames_[target].hit.boxes;
    dest_boxes.erase(std::remove_if(dest_boxes.begin(), dest_boxes.end(),
                                    [&](const auto& b) { return b.type == type; }),
                     dest_boxes.end());
    if (source) {
        dest_boxes.push_back(*source);
    }
    persist_changes();
    persist_pending_changes();
    invalidate_preview();
}

bool HitGeoFrameEditor::apply_hit_to_all_animations() {
    if (!context_.document || frames_.empty()) return false;
    apply_hit_to_animation();

    const std::string type = current_hitbox_type();
    const auto* source = current_hit_box();
    const auto ids = context_.document->animation_ids();
    for (const auto& id : ids) {
        auto payload_opt = context_.document->animation_payload_json(id);
        nlohmann::json payload = payload_opt.value_or(nlohmann::json::object());
        auto frames = parse_frames_from_payload(payload);
        if (frames.empty()) frames.push_back(MovementFrame{});
        for (auto& f : frames) {
            auto& boxes = f.hit.boxes;
            boxes.erase(std::remove_if(boxes.begin(), boxes.end(),
                                       [&](const auto& b) { return b.type == type; }),
                        boxes.end());
            if (source) {
                boxes.push_back(*source);
            }
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

std::string HitGeoFrameEditor::current_hitbox_type() const {
    int idx = std::clamp(selected_hitbox_type_index_, 0, static_cast<int>(kDamageTypeNames.size()) - 1);
    return kDamageTypeNames[static_cast<std::size_t>(idx)];
}

animation_update::FrameHitGeometry::HitBox* HitGeoFrameEditor::current_hit_box() {
    if (frames_.empty()) return nullptr;
    const int frame_index = clamp_index(selected_index_, static_cast<int>(frames_.size()));
    auto& frame = frames_[frame_index];
    const std::string type = current_hitbox_type();
    for (auto& box : frame.hit.boxes) {
        if (box.type == type) return &box;
    }
    return nullptr;
}

const animation_update::FrameHitGeometry::HitBox* HitGeoFrameEditor::current_hit_box() const {
    return const_cast<HitGeoFrameEditor*>(this)->current_hit_box();
}

animation_update::FrameHitGeometry::HitBox* HitGeoFrameEditor::ensure_hit_box_for_type(const std::string& type) {
    if (frames_.empty()) return nullptr;
    const int frame_index = clamp_index(selected_index_, static_cast<int>(frames_.size()));
    auto& frame = frames_[frame_index];
    for (auto& box : frame.hit.boxes) {
        if (box.type == type) {
            return &box;
        }
    }
    animation_update::FrameHitGeometry::HitBox box{};
    box.type = type;
    frame.hit.boxes.push_back(box);
    return &frame.hit.boxes.back();
}

void HitGeoFrameEditor::delete_hit_box_for_type(const std::string& type) {
    if (frames_.empty()) return;
    const int frame_index = clamp_index(selected_index_, static_cast<int>(frames_.size()));
    auto& frame = frames_[frame_index];
    frame.hit.boxes.erase(std::remove_if(frame.hit.boxes.begin(), frame.hit.boxes.end(),
                                         [&](const auto& b) { return b.type == type; }),
                          frame.hit.boxes.end());
    persist_changes();
}


bool HitGeoFrameEditor::build_hitbox_visual(const animation_update::FrameHitGeometry::HitBox& box,
                                            std::array<SDL_FPoint, 4>& corners,
                                            std::array<SDL_FPoint, 4>& edge_midpoints,
                                            SDL_FPoint& rotate_handle) const {
    if (!context_.assets || !context_.target) return false;
    const WarpedScreenGrid& cam = context_.camera ? *context_.camera : context_.assets->getView();
    SDL_Point anchor = asset_anchor_world();
    const float scale = asset_local_scale();
    if (scale <= 0.0001f) return false;

    FramePointResolver resolver(context_.target);
    const float cos_r = std::cos(box.rotation_degrees * kPi / 180.0f);
    const float sin_r = std::sin(box.rotation_degrees * kPi / 180.0f);
    auto rotate_vec = [&](SDL_FPoint v) -> SDL_FPoint {
        return SDL_FPoint{v.x * cos_r - v.y * sin_r, v.x * sin_r + v.y * cos_r};
    };
    auto to_screen = [&](SDL_FPoint local) -> SDL_FPoint {
        SDL_FPoint world{static_cast<float>(anchor.x) + local.x * scale, static_cast<float>(anchor.y) - local.y * scale};
        return cam.map_to_screen_f(world);
    };

    // Convert percent values back to local coordinates
    SDL_FPoint center_local{resolver.to_world_xy(box.center_x), resolver.to_world_xy(box.center_y)};
    std::array<SDL_FPoint, 4> local_corners = {
        SDL_FPoint{-box.half_width, box.half_height},
        SDL_FPoint{box.half_width, box.half_height},
        SDL_FPoint{box.half_width, -box.half_height},
        SDL_FPoint{-box.half_width, -box.half_height}};
    for (std::size_t i = 0; i < local_corners.size(); ++i) {
        SDL_FPoint rotated = rotate_vec(local_corners[i]);
        rotated.x += center_local.x;
        rotated.y += center_local.y;
        corners[i] = to_screen(rotated);
    }
    for (std::size_t i = 0; i < corners.size(); ++i) {
        const SDL_FPoint& a = corners[i];
        const SDL_FPoint& b = corners[(i + 1) % 4];
        edge_midpoints[i] = SDL_FPoint{(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f};
    }
    SDL_FPoint handle_local{0.0f, box.half_height + (20.0f / std::max(scale, 0.001f))};
    SDL_FPoint rotated_handle = rotate_vec(handle_local);
    rotated_handle.x += center_local.x;
    rotated_handle.y += center_local.y;
    rotate_handle = to_screen(rotated_handle);
    return true;
}

void HitGeoFrameEditor::render_hit_geometry(SDL_Renderer* renderer) const {
    if (!renderer || frames_.empty() || !context_.assets || !context_.target) return;
    const int frame_index = clamp_index(selected_index_, static_cast<int>(frames_.size()));
    const auto& frame = frames_[frame_index];
    if (frame.hit.boxes.empty()) return;
    const std::string type = current_hitbox_type();

    // Render hitbox rectangles for current frame
    for (const auto& box : frame.hit.boxes) {
        std::array<SDL_FPoint, 4> corners{};
        std::array<SDL_FPoint, 4> edge_midpoints{};
        SDL_FPoint rotate_handle{};
        if (!build_hitbox_visual(box, corners, edge_midpoints, rotate_handle)) continue;
        const bool selected = (box.type == type);

        SDL_Color fill = selected ? DMStyles::AccentButton().bg : DMStyles::HeaderButton().bg;
        fill.a = selected ? 90 : 45;
        SDL_Color outline = selected ? DMStyles::AccentButton().border : DMStyles::Border();

        SDL_Vertex verts[4];
        int indices[6] = {0, 1, 2, 0, 2, 3};
        for (int i = 0; i < 4; ++i) {
            verts[i].position.x = corners[i].x;
            verts[i].position.y = corners[i].y;
            verts[i].color = SDL_FColor{fill.r / 255.0f, fill.g / 255.0f, fill.b / 255.0f, fill.a / 255.0f};
            verts[i].tex_coord = SDL_FPoint{0.0f, 0.0f};
        }
        SDL_RenderGeometry(renderer, nullptr, verts, 4, indices, 6);
        SDL_SetRenderDrawColor(renderer, outline.r, outline.g, outline.b, 220);
        for (int i = 0; i < 4; ++i) {
            const SDL_FPoint& a = corners[i];
            const SDL_FPoint& b = corners[(i + 1) % 4];
            SDL_RenderLine(renderer, a.x, a.y, b.x, b.y);
        }
    }

    // Render center points for all frames' hitboxes of current type
    SDL_Point anchor = asset_anchor_world();
    const float scale = asset_local_scale();
    const WarpedScreenGrid& cam = context_.camera ? *context_.camera : context_.assets->getView();
    FramePointResolver resolver(context_.target);

    for (std::size_t i = 0; i < frames_.size(); ++i) {
        const auto& f = frames_[i];
        const animation_update::FrameHitGeometry::HitBox* box = nullptr;
        for (const auto& b : f.hit.boxes) {
            if (b.type == type) {
                box = &b;
                break;
            }
        }
        if (box) {
            // Convert percent values back to world coordinates
            const float center_x_local = resolver.to_world_xy(box->center_x);
            const float center_y_local = resolver.to_world_xy(box->center_y);
            SDL_FPoint center_world{static_cast<float>(anchor.x) + center_x_local * scale,
                                   static_cast<float>(anchor.y) - center_y_local * scale};
            SDL_FPoint center_screen = cam.map_to_screen_f(center_world);

            const bool is_current_frame = (static_cast<int>(i) == selected_index_);
            const bool is_selected = (is_current_frame &&
                                     selection_state_ &&
                                     selection_state_->target == SelectionTarget::HitboxCenter);
            const bool is_hovered = (static_cast<int>(i) == point_3d_editor_->get_hovered_point_index());

            if (is_current_frame) {
                point_3d_editor_->render_selectable_point(renderer, center_screen, is_selected, is_hovered);
            } else {
                point_3d_editor_->render_non_selectable_point(renderer, center_screen);
            }
        }
    }
}

SDL_Point HitGeoFrameEditor::asset_anchor_world() const {
    if (!context_.target) {
        return SDL_Point{0, 0};
    }
    return animation_update::detail::bottom_middle_for(*context_.target, context_.target->world_xz_point());
}

float HitGeoFrameEditor::asset_local_scale() const {
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

bool HitGeoFrameEditor::screen_to_local(SDL_Point screen, SDL_FPoint& out_local) const {
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

SDL_FPoint HitGeoFrameEditor::screen_to_world_point(const SDL_Point& screen) const {
    if (!context_.assets || !context_.target) return SDL_FPoint{0,0};
    const WarpedScreenGrid& cam = context_.camera ? *context_.camera : context_.assets->getView();
    return cam.screen_to_map(screen);
}

void HitGeoFrameEditor::refresh_selection_state() {
    if (!selection_state_ || !context_.assets || !context_.target) {
        return;
    }
    if (selection_state_->target != SelectionTarget::HitboxCenter) {
        return;
    }
    FramePointResolver resolver(context_.target);
    // Update parent height for Z percent display (in case scale changed)
    if (point_3d_editor_) {
        point_3d_editor_->set_parent_height(resolver.parent_height_px());
    }
    const auto* box = current_hit_box();
    if (!box) {
        selection_state_->target = SelectionTarget::None;
        return;
    }
    SDL_Point anchor = asset_anchor_world();
    float scale = asset_local_scale();
    // Convert percent values back to world coordinates
    const float center_x_local = resolver.to_world_xy(box->center_x);
    const float center_y_local = resolver.to_world_xy(box->center_y);
    SDL_FPoint world{
        static_cast<float>(anchor.x) + center_x_local * scale,
        static_cast<float>(anchor.y) - center_y_local * scale};
    const WarpedScreenGrid& cam = context_.camera ? *context_.camera : context_.assets->getView();
    SDL_FPoint screen = cam.map_to_screen_f(world);
    selection_state_->world_pos = world;
    const float base_z = resolver.base_world_depth();
    const float world_z = resolver.to_world_depth(box->center_z);
    selection_state_->world_z = world_z;
    selection_state_->screen_pos = round_point(screen);
    selection_state_->set_anchor_world(anchor, base_z);
}

bool HitGeoFrameEditor::ui_contains_point(const SDL_Point& pt) const {
    if (SDL_PointInRect(&pt, &nav_rect_)) return true;
    return tool_panel_ && tool_panel_->contains_point(pt);
}


}  // namespace devmode::frame_editors


