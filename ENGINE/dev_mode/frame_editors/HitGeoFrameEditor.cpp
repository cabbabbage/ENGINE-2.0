#include "HitGeoFrameEditor.hpp"

#include <SDL.h>

#include <algorithm>
#include <array>
#include <cmath>

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

        point_3d_editor_->set_on_point_selected([this](int /*index*/) {
            if (selection_state_) {
                selection_state_->target = SelectionTarget::HitboxCenter;
            }
            refresh_selection_state();
        });

        point_3d_editor_->set_on_position_changed([this](const SDL_FPoint& new_world_pos, float new_world_z) {
            auto* box = current_hit_box();
            if (!box) return;

            SDL_Point anchor = asset_anchor_world();
            float scale = asset_local_scale();

            box->center_x = (new_world_pos.x - static_cast<float>(anchor.x)) / scale;
            box->center_y = (static_cast<float>(anchor.y) - new_world_pos.y) / scale;
            box->center_z = (new_world_z - (context_.target ? context_.target->world_z_offset() : 0.0f)) / scale;

            persist_changes();
            refresh_selection_state();
        });

        point_3d_editor_->set_on_coordinates_changed([this]() {
            auto* box = current_hit_box();
            if (!box || !selection_state_) return;

            SDL_Point anchor = asset_anchor_world();
            float scale = asset_local_scale();

            box->center_x = (selection_state_->world_pos.x - static_cast<float>(anchor.x)) / scale;
            box->center_y = (static_cast<float>(anchor.y) - selection_state_->world_pos.y) / scale;
            box->center_z = (selection_state_->world_z - (context_.target ? context_.target->world_z_offset() : 0.0f)) / scale;

            persist_changes();
        });
    }
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
    btn_back_ = std::make_unique<DMButton>("Back", &DMStyles::HeaderButton(), 80, DMButton::height());
    btn_prev_frame_ = std::make_unique<DMButton>("<", &DMStyles::AccentButton(), 36, DMButton::height());
    btn_next_frame_ = std::make_unique<DMButton>(">", &DMStyles::AccentButton(), 36, DMButton::height());
    btn_add_remove_ = std::make_unique<DMButton>("Add Hit Box", &DMStyles::AccentButton(), 150, DMButton::height());
    btn_copy_next_ = std::make_unique<DMButton>("Copy To Next", &DMStyles::HeaderButton(), 150, DMButton::height());
    btn_apply_all_ = std::make_unique<DMButton>("Apply To All Frames", &DMStyles::HeaderButton(), 180, DMButton::height());

    refresh_hitbox_form();
    refresh_selection_state();
}

void HitGeoFrameEditor::end() {
    if (manifest_txn_.active()) {
        manifest_txn_.commit();
    }
    frames_.clear();
    if (selection_state_) {
        selection_state_->reset();
        selection_state_ = nullptr;
    }
    point_3d_editor_ = nullptr;
    dd_hitbox_type_.reset();
    btn_back_.reset();
    btn_prev_frame_.reset();
    btn_next_frame_.reset();
    btn_add_remove_.reset();
    btn_copy_next_.reset();
    btn_apply_all_.reset();
    wants_close_ = false;
}

bool HitGeoFrameEditor::handle_event(const SDL_Event& e) {
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

    if (btn_prev_frame_ && btn_prev_frame_->handle_event(e)) {
        select_frame(selected_index_ - 1);
        consumed = true;
    }
    if (btn_next_frame_ && btn_next_frame_->handle_event(e)) {
        select_frame(selected_index_ + 1);
        consumed = true;
    }
    if (btn_back_ && btn_back_->handle_event(e)) {
        wants_close_ = true;
        consumed = true;
    }
    if (dd_hitbox_type_ && dd_hitbox_type_->handle_event(e)) {
        selected_hitbox_type_index_ = std::clamp(dd_hitbox_type_->selected(), 0, static_cast<int>(kDamageTypeNames.size()) - 1);
        refresh_hitbox_form();
        consumed = true;
    }
    if (btn_add_remove_ && btn_add_remove_->handle_event(e)) {
        auto* box = current_hit_box();
        const std::string type = current_hitbox_type();
        if (box) {
            delete_hit_box_for_type(type);
        } else {
            ensure_hit_box_for_type(type);
        }
        refresh_hitbox_form();
        consumed = true;
    }
    if (btn_copy_next_ && btn_copy_next_->handle_event(e)) {
        copy_hit_box_to_next_frame();
        consumed = true;
    }
    if (btn_apply_all_ && btn_apply_all_->handle_event(e)) {
        apply_hit_to_all_frames();
        consumed = true;
    }

    if (e.type == SDL_KEYDOWN) {
        // For HitGeoFrameEditor, arrow keys navigate frames (one point per frame)
        if (e.key.keysym.sym == SDLK_LEFT) {
            int new_index = selected_index_ - 1;
            if (new_index >= 0) {
                select_frame(new_index);
                if (point_3d_editor_) {
                    point_3d_editor_->set_selected_point_index(0);  // Always point 0 (hitbox center)
                }
            }
            consumed = true;
        } else if (e.key.keysym.sym == SDLK_RIGHT) {
            int new_index = selected_index_ + 1;
            select_frame(new_index);
            if (point_3d_editor_) {
                point_3d_editor_->set_selected_point_index(0);  // Always point 0 (hitbox center)
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
        const auto* box = current_hit_box();
        std::vector<SDL_FPoint> point_screens;
        if (box) {
            std::array<SDL_FPoint, 4> corners{};
            std::array<SDL_FPoint, 4> edge_midpoints{};
            SDL_FPoint rotate_handle{};
            if (build_hitbox_visual(*box, corners, edge_midpoints, rotate_handle)) {
                // Point 0 is center
                SDL_Point anchor = asset_anchor_world();
                const float scale = asset_local_scale();
                const WarpedScreenGrid& cam = context_.camera ? *context_.camera : context_.assets->getView();
                SDL_FPoint world{static_cast<float>(anchor.x) + box->center_x * scale, static_cast<float>(anchor.y) - box->center_y * scale};
                point_screens.push_back(cam.map_to_screen_f(world));
            }
        }

        // Only consume event if point editor actually handled it
        consumed = point_3d_editor_->handle_mouse_event(e, point_screens, [this](const SDL_Point& p) {
                return screen_to_world_point(p);
            });
    }

    return consumed;
}

void HitGeoFrameEditor::update(const Input&, float) {
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
    if (btn_back_) btn_back_->render(renderer);
    if (btn_prev_frame_) btn_prev_frame_->render(renderer);
    if (btn_next_frame_) btn_next_frame_->render(renderer);
    if (dd_hitbox_type_) dd_hitbox_type_->render(renderer);
    if (btn_add_remove_) btn_add_remove_->render(renderer);
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

void HitGeoFrameEditor::layout_ui(SDL_Renderer* renderer) const {
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

    if (btn_prev_frame_ && btn_next_frame_) {
        int half_w = (inner_w - DMSpacing::small_gap()) / 2;
        SDL_Rect left{x + padding, cursor_y, half_w, DMButton::height()};
        SDL_Rect right{x + padding + half_w + DMSpacing::small_gap(), cursor_y, half_w, DMButton::height()};
        btn_prev_frame_->set_rect(left);
        btn_next_frame_->set_rect(right);
        cursor_y += DMButton::height() + DMSpacing::small_gap();
    }

    if (dd_hitbox_type_) {
        dd_hitbox_type_->set_rect(place_row(DMDropdown::height()));
    }

    if (btn_add_remove_ || btn_copy_next_) {
        SDL_Rect row = place_row(DMButton::height());
        int button_count = 0;
        if (btn_add_remove_) ++button_count;
        if (btn_copy_next_) ++button_count;
        const int total_gap = DMSpacing::small_gap() * std::max(0, button_count - 1);
        const int button_w = (inner_w - total_gap) / std::max(1, button_count);
        int offset_x = row.x;
        if (btn_add_remove_) {
            btn_add_remove_->set_rect(SDL_Rect{offset_x, row.y, button_w, row.h});
            offset_x += button_w + DMSpacing::small_gap();
        }
        if (btn_copy_next_) {
            btn_copy_next_->set_rect(SDL_Rect{offset_x, row.y, button_w, row.h});
        }
    }

    if (btn_apply_all_) {
        btn_apply_all_->set_rect(place_row(DMButton::height()));
    }
    ui_rect_.h = cursor_y - y;
}

void HitGeoFrameEditor::select_frame(int index) {
    selected_index_ = clamp_index(index, static_cast<int>(frames_.size()));
    refresh_hitbox_form();
    refresh_selection_state();
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
    if (manifest_txn_.active()) {
        manifest_txn_.commit();
    }
    refresh_hitbox_form();
    refresh_selection_state();
}

float HitGeoFrameEditor::base_world_z() const {
    return context_.target ? context_.target->world_z_offset() : 0.0f;
}

void HitGeoFrameEditor::apply_hit_to_all_frames() {
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
}

void HitGeoFrameEditor::copy_hit_box_to_next_frame() {
    if (frames_.empty()) return;
    const int next_index = selected_index_ + 1;
    if (next_index >= static_cast<int>(frames_.size())) {
        return;
    }
    const std::string type = current_hitbox_type();
    const auto* source = current_hit_box();
    if (!source) return;
    auto& dest_boxes = frames_[next_index].hit.boxes;
    dest_boxes.erase(std::remove_if(dest_boxes.begin(), dest_boxes.end(),
                                    [&](const auto& b) { return b.type == type; }),
                     dest_boxes.end());
    dest_boxes.push_back(*source);
    persist_changes();
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

    const float cos_r = std::cos(box.rotation_degrees * static_cast<float>(M_PI) / 180.0f);
    const float sin_r = std::sin(box.rotation_degrees * static_cast<float>(M_PI) / 180.0f);
    auto rotate_vec = [&](SDL_FPoint v) -> SDL_FPoint {
        return SDL_FPoint{v.x * cos_r - v.y * sin_r, v.x * sin_r + v.y * cos_r};
    };
    auto to_screen = [&](SDL_FPoint local) -> SDL_FPoint {
        SDL_FPoint world{static_cast<float>(anchor.x) + local.x * scale, static_cast<float>(anchor.y) - local.y * scale};
        return cam.map_to_screen_f(world);
    };

    SDL_FPoint center_local{box.center_x, box.center_y};
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
            verts[i].color = fill;
            verts[i].tex_coord = SDL_FPoint{0.0f, 0.0f};
        }
        SDL_RenderGeometry(renderer, nullptr, verts, 4, indices, 6);
        SDL_SetRenderDrawColor(renderer, outline.r, outline.g, outline.b, 220);
        for (int i = 0; i < 4; ++i) {
            const SDL_FPoint& a = corners[i];
            const SDL_FPoint& b = corners[(i + 1) % 4];
            SDL_RenderDrawLineF(renderer, a.x, a.y, b.x, b.y);
        }
        if (selected) {
            // Render center point
            SDL_Point anchor = asset_anchor_world();
            const float scale = asset_local_scale();
            const WarpedScreenGrid& cam = context_.camera ? *context_.camera : context_.assets->getView();
            SDL_FPoint center_world{static_cast<float>(anchor.x) + box.center_x * scale, static_cast<float>(anchor.y) - box.center_y * scale};
            SDL_FPoint center_screen = cam.map_to_screen_f(center_world);
            
            point_3d_editor_->render_axis_point(renderer, center_screen,
                                              selection_state_ ? selection_state_->axis : AdjustmentAxis::X,
                                              true);
        }
    }
}

SDL_Point HitGeoFrameEditor::asset_anchor_world() const {
    if (!context_.target) {
        return SDL_Point{0, 0};
    }
    return animation_update::detail::bottom_middle_for(*context_.target, context_.target->pos);
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
    const auto* box = current_hit_box();
    if (!box) {
        selection_state_->target = SelectionTarget::None;
        return;
    }
    SDL_Point anchor = asset_anchor_world();
    float scale = asset_local_scale();
    SDL_FPoint world{
        static_cast<float>(anchor.x) + box->center_x * scale,
        static_cast<float>(anchor.y) - box->center_y * scale};
    const WarpedScreenGrid& cam = context_.camera ? *context_.camera : context_.assets->getView();
    SDL_FPoint screen = cam.map_to_screen_f(world);
    selection_state_->world_pos = world;
    selection_state_->world_z = base_world_z() + box->center_z * scale;
    selection_state_->screen_pos = round_point(screen);
}

bool HitGeoFrameEditor::ui_contains_point(const SDL_Point& pt) const {
    return SDL_PointInRect(&pt, &ui_rect_) == SDL_TRUE;
}


}  // namespace devmode::frame_editors
