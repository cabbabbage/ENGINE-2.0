#include "HitGeoFrameEditor.hpp"

#include <SDL.h>

#include <algorithm>
#include <array>
#include <cmath>

#include "animation_update/animation_update.hpp"
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
    axis_adjuster_ = context.axis_adjuster;
    if (selection_state_) {
        selection_state_->reset();
    }
    if (axis_adjuster_) {
        axis_adjuster_->reset_axis(AdjustmentAxis::X);
    }
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
    btn_prev_frame_ = std::make_unique<DMButton>("<", &DMStyles::AccentButton(), 36, DMButton::height());
    btn_next_frame_ = std::make_unique<DMButton>(">", &DMStyles::AccentButton(), 36, DMButton::height());
    btn_add_remove_ = std::make_unique<DMButton>("Add Hit Box", &DMStyles::AccentButton(), 150, DMButton::height());
    btn_copy_next_ = std::make_unique<DMButton>("Copy To Next", &DMStyles::HeaderButton(), 150, DMButton::height());
    btn_apply_all_ = std::make_unique<DMButton>("Apply To All Frames", &DMStyles::HeaderButton(), 180, DMButton::height());
    tb_center_x_ = std::make_unique<DMTextBox>("Center X", "0");
    tb_center_y_ = std::make_unique<DMTextBox>("Center Y", "0");
    tb_width_ = std::make_unique<DMTextBox>("Width", "0");
    tb_height_ = std::make_unique<DMTextBox>("Height", "0");
    tb_rotation_ = std::make_unique<DMTextBox>("Rotation", "0");

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
    axis_adjuster_ = nullptr;
    dd_hitbox_type_.reset();
    btn_prev_frame_.reset();
    btn_next_frame_.reset();
    btn_add_remove_.reset();
    btn_copy_next_.reset();
    btn_apply_all_.reset();
    tb_center_x_.reset();
    tb_center_y_.reset();
    tb_width_.reset();
    tb_height_.reset();
    tb_rotation_.reset();
}

bool HitGeoFrameEditor::handle_event(const SDL_Event& e) {
    bool consumed = false;
    apply_text_fields();

    if (btn_prev_frame_ && btn_prev_frame_->handle_event(e)) {
        select_frame(selected_index_ - 1);
        consumed = true;
    }
    if (btn_next_frame_ && btn_next_frame_->handle_event(e)) {
        select_frame(selected_index_ + 1);
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

    if (tb_center_x_ && tb_center_x_->handle_event(e)) { apply_text_fields(); consumed = true; }
    if (tb_center_y_ && tb_center_y_->handle_event(e)) { apply_text_fields(); consumed = true; }
    if (tb_width_ && tb_width_->handle_event(e)) { apply_text_fields(); consumed = true; }
    if (tb_height_ && tb_height_->handle_event(e)) { apply_text_fields(); consumed = true; }
    if (tb_rotation_ && tb_rotation_->handle_event(e)) { apply_text_fields(); consumed = true; }

    if (e.type == SDL_KEYDOWN) {
        if (e.key.keysym.sym == SDLK_LEFT) {
            select_frame(selected_index_ - 1);
            consumed = true;
        } else if (e.key.keysym.sym == SDLK_RIGHT) {
            select_frame(selected_index_ + 1);
            consumed = true;
        }
    }

    if (!context_.assets || !context_.target) {
        return consumed;
    }

    if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
        SDL_Point mp{e.button.x, e.button.y};
        if (ui_contains_point(mp)) {
            return true;
        }
        if (begin_hitbox_drag(mp)) {
            consumed = true;
        }
    } else if (e.type == SDL_MOUSEBUTTONUP && e.button.button == SDL_BUTTON_LEFT) {
        if (dragging_hitbox_) {
            end_hitbox_drag(true);
            consumed = true;
        }
    } else if (e.type == SDL_MOUSEMOTION && dragging_hitbox_) {
        update_hitbox_drag(SDL_Point{e.motion.x, e.motion.y});
        consumed = true;
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
    if (btn_prev_frame_) btn_prev_frame_->render(renderer);
    if (btn_next_frame_) btn_next_frame_->render(renderer);
    if (dd_hitbox_type_) dd_hitbox_type_->render(renderer);
    if (btn_add_remove_) btn_add_remove_->render(renderer);
    if (btn_copy_next_) btn_copy_next_->render(renderer);
    if (tb_center_x_) tb_center_x_->render(renderer);
    if (tb_center_y_) tb_center_y_->render(renderer);
    if (tb_width_) tb_width_->render(renderer);
    if (tb_height_) tb_height_->render(renderer);
    if (tb_rotation_) tb_rotation_->render(renderer);
    if (btn_apply_all_) btn_apply_all_->render(renderer);
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

    auto place_pair = [&](DMTextBox* a, DMTextBox* b) {
        if (!a || !b) return;
        int half_w = (inner_w - DMSpacing::small_gap()) / 2;
        int h_a = a->height_for_width(half_w);
        int h_b = b->height_for_width(half_w);
        int h = std::max(h_a, h_b);
        a->set_rect(SDL_Rect{x + padding, cursor_y, half_w, h});
        b->set_rect(SDL_Rect{x + padding + half_w + DMSpacing::small_gap(), cursor_y, half_w, h});
        cursor_y += h + DMSpacing::small_gap();
    };

    place_pair(tb_center_x_.get(), tb_center_y_.get());
    place_pair(tb_width_.get(), tb_height_.get());
    if (tb_rotation_) {
        tb_rotation_->set_rect(place_row(tb_rotation_->height_for_width(inner_w)));
    }
    if (btn_apply_all_) {
        btn_apply_all_->set_rect(place_row(DMButton::height()));
    }
    ui_rect_.h = cursor_y - y;
}

void HitGeoFrameEditor::select_frame(int index) {
    selected_index_ = clamp_index(index, static_cast<int>(frames_.size()));
    dragging_hitbox_ = false;
    refresh_hitbox_form();
    refresh_selection_state();
}

void HitGeoFrameEditor::refresh_hitbox_form() const {
    const auto* box = current_hit_box();
    if (box) {
        auto set_field = [](DMTextBox* tb, std::string& cache, const std::string& value) {
            if (!tb || tb->is_editing()) return;
            if (cache != value) {
                tb->set_value(value);
                cache = value;
            }
        };
        set_field(tb_center_x_.get(), last_center_x_, std::to_string(static_cast<int>(std::lround(box->center_x))));
        set_field(tb_center_y_.get(), last_center_y_, std::to_string(static_cast<int>(std::lround(box->center_y))));
        set_field(tb_width_.get(), last_width_, std::to_string(static_cast<int>(std::lround(box->half_width * 2.0f))));
        set_field(tb_height_.get(), last_height_, std::to_string(static_cast<int>(std::lround(box->half_height * 2.0f))));
        set_field(tb_rotation_.get(), last_rotation_, std::to_string(static_cast<int>(std::lround(box->rotation_degrees))));
        if (btn_add_remove_) btn_add_remove_->set_text("Delete Hit Box");
    } else {
        if (tb_center_x_ && !tb_center_x_->is_editing()) { tb_center_x_->set_value("0"); last_center_x_ = "0"; }
        if (tb_center_y_ && !tb_center_y_->is_editing()) { tb_center_y_->set_value("0"); last_center_y_ = "0"; }
        if (tb_width_ && !tb_width_->is_editing()) { tb_width_->set_value("0"); last_width_ = "0"; }
        if (tb_height_ && !tb_height_->is_editing()) { tb_height_->set_value("0"); last_height_ = "0"; }
        if (tb_rotation_ && !tb_rotation_->is_editing()) { tb_rotation_->set_value("0"); last_rotation_ = "0"; }
        if (btn_add_remove_) btn_add_remove_->set_text("Add Hit Box");
    }
}

void HitGeoFrameEditor::apply_text_fields() {
    auto* box = current_hit_box();
    if (!box) {
        return;
    }
    bool changed = false;
    if (tb_center_x_) {
        float v = parse_float(tb_center_x_->value(), box->center_x);
        if (std::fabs(v - box->center_x) > 0.001f) { box->center_x = v; changed = true; }
        last_center_x_ = tb_center_x_->value();
    }
    if (tb_center_y_) {
        float v = parse_float(tb_center_y_->value(), box->center_y);
        if (std::fabs(v - box->center_y) > 0.001f) { box->center_y = v; changed = true; }
        last_center_y_ = tb_center_y_->value();
    }
    if (tb_width_) {
        float v = parse_float(tb_width_->value(), box->half_width * 2.0f);
        if (v < 0.0f) v = 0.0f;
        float half = v * 0.5f;
        if (std::fabs(half - box->half_width) > 0.001f) { box->half_width = half; changed = true; }
        last_width_ = tb_width_->value();
    }
    if (tb_height_) {
        float v = parse_float(tb_height_->value(), box->half_height * 2.0f);
        if (v < 0.0f) v = 0.0f;
        float half = v * 0.5f;
        if (std::fabs(half - box->half_height) > 0.001f) { box->half_height = half; changed = true; }
        last_height_ = tb_height_->value();
    }
    if (tb_rotation_) {
        float v = parse_float(tb_rotation_->value(), box->rotation_degrees);
        if (std::fabs(v - box->rotation_degrees) > 0.001f) { box->rotation_degrees = v; changed = true; }
        last_rotation_ = tb_rotation_->value();
    }
    if (changed) {
        persist_changes();
    }
}

void HitGeoFrameEditor::persist_changes() {
    if (manifest_txn_.active()) {
        manifest_txn_.commit();
    }
    refresh_hitbox_form();
    refresh_selection_state();
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

bool HitGeoFrameEditor::begin_hitbox_drag(SDL_Point mouse) {
    auto* box = current_hit_box();
    if (!box) return false;
    std::array<SDL_FPoint, 4> corners{};
    std::array<SDL_FPoint, 4> edge_midpoints{};
    SDL_FPoint rotate_handle{};
    if (!build_hitbox_visual(*box, corners, edge_midpoints, rotate_handle)) return false;
    active_handle_ = HitHandle::None;
    const int handle_size = 12;
    auto point_in_rect = [&](const SDL_FPoint& center) {
        SDL_Rect r{static_cast<int>(std::round(center.x)) - handle_size / 2,
                   static_cast<int>(std::round(center.y)) - handle_size / 2, handle_size, handle_size};
        return SDL_PointInRect(&mouse, &r) == SDL_TRUE;
    };
    SDL_FPoint mouse_f{static_cast<float>(mouse.x), static_cast<float>(mouse.y)};
    const float rotate_radius = 12.0f;
    if (dist_sq(mouse_f, rotate_handle) <= rotate_radius * rotate_radius) {
        active_handle_ = HitHandle::Rotate;
    } else {
        if (point_in_rect(edge_midpoints[3])) active_handle_ = HitHandle::Left;
        else if (point_in_rect(edge_midpoints[1])) active_handle_ = HitHandle::Right;
        else if (point_in_rect(edge_midpoints[0])) active_handle_ = HitHandle::Top;
        else if (point_in_rect(edge_midpoints[2])) active_handle_ = HitHandle::Bottom;
        else {
            bool inside = false;
            for (int i = 0, j = 3; i < 4; j = i++) {
                const SDL_FPoint& a = corners[i];
                const SDL_FPoint& b = corners[j];
                const bool intersect = ((a.y > mouse_f.y) != (b.y > mouse_f.y)) &&
                                       (mouse_f.x < (b.x - a.x) * (mouse_f.y - a.y) / (b.y - a.y + 0.0001f) + a.x);
                if (intersect) inside = !inside;
            }
            if (inside) {
                active_handle_ = HitHandle::Move;
            }
        }
    }
    if (active_handle_ == HitHandle::None) {
        return false;
    }
    dragging_hitbox_ = true;
    drag_start_mouse_ = mouse;
    drag_start_box_ = *box;
    drag_left_ = -box->half_width;
    drag_right_ = box->half_width;
    drag_top_ = box->half_height;
    drag_bottom_ = -box->half_height;
    SDL_FPoint local_mouse{};
    if (!screen_to_local(mouse, local_mouse)) {
        dragging_hitbox_ = false;
        active_handle_ = HitHandle::None;
        return false;
    }
    drag_grab_offset_.x = local_mouse.x - box->center_x;
    drag_grab_offset_.y = local_mouse.y - box->center_y;
    return true;
}

void HitGeoFrameEditor::update_hitbox_drag(SDL_Point mouse) {
    if (!dragging_hitbox_) return;
    auto* box = current_hit_box();
    if (!box) return;
    SDL_FPoint local{};
    if (!screen_to_local(mouse, local)) {
        return;
    }
    constexpr float kMinHalf = 2.0f;
    const float rotation = drag_start_box_.rotation_degrees;
    const float cos_r = std::cos(rotation * static_cast<float>(M_PI) / 180.0f);
    const float sin_r = std::sin(rotation * static_cast<float>(M_PI) / 180.0f);
    auto rotate_to_box = [&](float dx, float dy) -> SDL_FPoint {
        return SDL_FPoint{dx * cos_r + dy * sin_r, -dx * sin_r + dy * cos_r};
    };
    auto rotate_to_world = [&](SDL_FPoint v) -> SDL_FPoint {
        return SDL_FPoint{v.x * cos_r - v.y * sin_r, v.x * sin_r + v.y * cos_r};
    };
    SDL_FPoint delta{local.x - drag_start_box_.center_x, local.y - drag_start_box_.center_y};
    SDL_FPoint aligned = rotate_to_box(delta.x, delta.y);
    switch (active_handle_) {
        case HitHandle::Move: {
            SDL_FPoint new_center{local.x - drag_grab_offset_.x, local.y - drag_grab_offset_.y};
            if (selection_state_) {
                if (selection_state_->axis == AdjustmentAxis::X) {
                    new_center.y = drag_start_box_.center_y;
                } else if (selection_state_->axis == AdjustmentAxis::Y) {
                    new_center.x = drag_start_box_.center_x;
                }
            }
            box->center_x = new_center.x;
            box->center_y = new_center.y;
            break;
        }
        case HitHandle::Left:
        case HitHandle::Right: {
            float left = drag_left_;
            float right = drag_right_;
            if (active_handle_ == HitHandle::Left) {
                left = std::min(aligned.x, right - kMinHalf * 2.0f);
            } else {
                right = std::max(aligned.x, left + kMinHalf * 2.0f);
            }
            const float width = std::max(kMinHalf * 2.0f, right - left);
            const float center_offset = (right + left) * 0.5f;
            SDL_FPoint offset_world = rotate_to_world(SDL_FPoint{center_offset, 0.0f});
            box->center_x = drag_start_box_.center_x + offset_world.x;
            box->center_y = drag_start_box_.center_y + offset_world.y;
            box->half_width = width * 0.5f;
            break;
        }
        case HitHandle::Top:
        case HitHandle::Bottom: {
            float bottom = drag_bottom_;
            float top = drag_top_;
            if (active_handle_ == HitHandle::Top) {
                top = std::max(aligned.y, bottom + kMinHalf * 2.0f);
            } else {
                bottom = std::min(aligned.y, top - kMinHalf * 2.0f);
            }
            const float height = std::max(kMinHalf * 2.0f, top - bottom);
            const float center_offset = (top + bottom) * 0.5f;
            SDL_FPoint offset_world = rotate_to_world(SDL_FPoint{0.0f, center_offset});
            box->center_x = drag_start_box_.center_x + offset_world.x;
            box->center_y = drag_start_box_.center_y + offset_world.y;
            box->half_height = height * 0.5f;
            break;
        }
        case HitHandle::Rotate: {
            SDL_FPoint rel{local.x - box->center_x, local.y - box->center_y};
            box->rotation_degrees = std::atan2(rel.y, rel.x) * 180.0f / static_cast<float>(M_PI);
            break;
        }
        case HitHandle::None:
        default:
            break;
    }
    refresh_hitbox_form();
    persist_changes();
}

void HitGeoFrameEditor::end_hitbox_drag(bool commit) {
    if (!dragging_hitbox_) return;
    dragging_hitbox_ = false;
    if (!commit) {
        if (auto* box = current_hit_box()) {
            *box = drag_start_box_;
            refresh_hitbox_form();
        }
        return;
    }
    refresh_hitbox_form();
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

        bool hovered_any = false;
        int hovered_edge_index = -1;
        bool hovered_rotate = false;
        if (selected) {
            SDL_Point mp{0, 0};
            SDL_GetMouseState(&mp.x, &mp.y);
            const int handle_size = 12;
            auto point_in_rect = [&](const SDL_FPoint& center) {
                SDL_Rect r{static_cast<int>(std::round(center.x)) - handle_size / 2,
                           static_cast<int>(std::round(center.y)) - handle_size / 2, handle_size, handle_size};
                return SDL_PointInRect(&mp, &r) == SDL_TRUE;
            };

            SDL_FPoint mpf{static_cast<float>(mp.x), static_cast<float>(mp.y)};
            const float rotate_radius = 12.0f;
            if (dist_sq(mpf, rotate_handle) <= rotate_radius * rotate_radius) {
                hovered_any = true;
                hovered_rotate = true;
            } else {
                if (point_in_rect(edge_midpoints[3])) { hovered_any = true; hovered_edge_index = 3; }
                else if (point_in_rect(edge_midpoints[1])) { hovered_any = true; hovered_edge_index = 1; }
                else if (point_in_rect(edge_midpoints[0])) { hovered_any = true; hovered_edge_index = 0; }
                else if (point_in_rect(edge_midpoints[2])) { hovered_any = true; hovered_edge_index = 2; }
            }
        }

        SDL_Color fill = selected ? DMStyles::AccentButton().bg : DMStyles::HeaderButton().bg;
        fill.a = selected ? 90 : 45;
        SDL_Color outline = selected ? DMStyles::AccentButton().border : DMStyles::Border();
        if (selected && hovered_any) {
            outline = SDL_Color{255, 255, 255, 255};
        }
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
            const int base_handle_size = 10;
            for (int i = 0; i < 4; ++i) {
                const bool is_hovered_handle = (i == hovered_edge_index);
                const int handle_size = is_hovered_handle ? (base_handle_size + 2) : base_handle_size;
                SDL_FRect r{edge_midpoints[i].x - handle_size * 0.5f,
                            edge_midpoints[i].y - handle_size * 0.5f,
                            static_cast<float>(handle_size),
                            static_cast<float>(handle_size)};
                SDL_Color node_col = is_hovered_handle ? SDL_Color{255, 255, 255, 255}
                                                       : DMStyles::AccentButton().hover_bg;
                SDL_SetRenderDrawColor(renderer, node_col.r, node_col.g, node_col.b, 255);
                SDL_RenderFillRectF(renderer, &r);
            }

            const SDL_FPoint top_mid = edge_midpoints[0];
            SDL_SetRenderDrawColor(renderer, (hovered_rotate ? 255 : DMStyles::AccentButton().hover_bg.r),
                                   (hovered_rotate ? 255 : DMStyles::AccentButton().hover_bg.g),
                                   (hovered_rotate ? 255 : DMStyles::AccentButton().hover_bg.b), 255);
            SDL_RenderDrawLineF(renderer, top_mid.x, top_mid.y, rotate_handle.x, rotate_handle.y);
            const float radius = 8.0f;
            for (int i = 0; i < 16; ++i) {
                const float a = (static_cast<float>(i) / 16.0f) * 2.0f * static_cast<float>(M_PI);
                const float b = (static_cast<float>(i + 1) / 16.0f) * 2.0f * static_cast<float>(M_PI);
                SDL_RenderDrawLineF(renderer, rotate_handle.x + std::cos(a) * radius,
                                    rotate_handle.y + std::sin(a) * radius,
                                    rotate_handle.x + std::cos(b) * radius,
                                    rotate_handle.y + std::sin(b) * radius);
            }
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

void HitGeoFrameEditor::refresh_selection_state() {
    if (!selection_state_ || !context_.assets || !context_.target) {
        return;
    }
    const auto* box = current_hit_box();
    if (!box) {
        selection_state_->target = SelectionTarget::None;
        selection_state_->attack_vector_index = -1;
        return;
    }
    selection_state_->target = SelectionTarget::HitboxCenter;
    SDL_Point anchor = asset_anchor_world();
    SDL_FPoint world{
        static_cast<float>(anchor.x) + box->center_x * asset_local_scale(),
        static_cast<float>(anchor.y) - box->center_y * asset_local_scale()};
    const WarpedScreenGrid& cam = context_.camera ? *context_.camera : context_.assets->getView();
    SDL_FPoint screen = cam.map_to_screen_f(world);
    selection_state_->world_pos = world;
    selection_state_->screen_pos = round_point(screen);
}

bool HitGeoFrameEditor::ui_contains_point(const SDL_Point& p) const {
    return SDL_PointInRect(&p, &ui_rect_) == SDL_TRUE;
}


}  // namespace devmode::frame_editors
