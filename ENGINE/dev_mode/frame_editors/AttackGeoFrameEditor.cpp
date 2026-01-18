#include "AttackGeoFrameEditor.hpp"

#include <SDL.h>

#include <algorithm>
#include <cmath>
#include <utility>

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

int parse_int(const std::string& text, int fallback) {
    try {
        return std::stoi(text);
    } catch (...) {
        return fallback;
    }
}

}  // namespace

void AttackGeoFrameEditor::begin(const FrameEditorContext& context) {
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
    selected_attack_type_index_ = 1;
    selected_attack_vector_indices_.fill(-1);
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

    std::vector<std::string> attack_type_labels;
    attack_type_labels.reserve(kDamageTypeNames.size());
    for (const char* type : kDamageTypeNames) {
        attack_type_labels.emplace_back(type);
    }

    dd_attack_type_ = std::make_unique<DMDropdown>("Attack Type",
                                                   attack_type_labels,
                                                   std::clamp(selected_attack_type_index_, 0,
                                                              static_cast<int>(attack_type_labels.size()) - 1));
    btn_prev_frame_ = std::make_unique<DMButton>("<", &DMStyles::AccentButton(), 36, DMButton::height());
    btn_next_frame_ = std::make_unique<DMButton>(">", &DMStyles::AccentButton(), 36, DMButton::height());
    btn_add_remove_ = std::make_unique<DMButton>("Add Attack", &DMStyles::AccentButton(), 150, DMButton::height());
    btn_delete_ = std::make_unique<DMButton>("Delete Attack", &DMStyles::DeleteButton(), 150, DMButton::height());
    btn_copy_next_ = std::make_unique<DMButton>("Copy To Next", &DMStyles::HeaderButton(), 150, DMButton::height());
    btn_apply_all_ = std::make_unique<DMButton>("Apply To All Frames", &DMStyles::HeaderButton(), 180, DMButton::height());
    tb_start_x_ = std::make_unique<DMTextBox>("Start X", "0");
    tb_start_y_ = std::make_unique<DMTextBox>("Start Y", "0");
    tb_control_x_ = std::make_unique<DMTextBox>("Control X", "0");
    tb_control_y_ = std::make_unique<DMTextBox>("Control Y", "0");
    tb_end_x_ = std::make_unique<DMTextBox>("End X", "0");
    tb_end_y_ = std::make_unique<DMTextBox>("End Y", "0");
    tb_damage_ = std::make_unique<DMTextBox>("Damage", "0");

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
    axis_adjuster_ = nullptr;
    dd_attack_type_.reset();
    btn_prev_frame_.reset();
    btn_next_frame_.reset();
    btn_add_remove_.reset();
    btn_delete_.reset();
    btn_copy_next_.reset();
    btn_apply_all_.reset();
    tb_start_x_.reset();
    tb_start_y_.reset();
    tb_control_x_.reset();
    tb_control_y_.reset();
    tb_end_x_.reset();
    tb_end_y_.reset();
    tb_damage_.reset();
}

bool AttackGeoFrameEditor::handle_event(const SDL_Event& e) {
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
    if (dd_attack_type_ && dd_attack_type_->handle_event(e)) {
        selected_attack_type_index_ = std::clamp(dd_attack_type_->selected(), 0, static_cast<int>(kDamageTypeNames.size()) - 1);
        clamp_attack_selection();
        refresh_attack_form();
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

    if (tb_start_x_ && tb_start_x_->handle_event(e)) { apply_text_fields(); consumed = true; }
    if (tb_start_y_ && tb_start_y_->handle_event(e)) { apply_text_fields(); consumed = true; }
    if (tb_control_x_ && tb_control_x_->handle_event(e)) { apply_text_fields(); consumed = true; }
    if (tb_control_y_ && tb_control_y_->handle_event(e)) { apply_text_fields(); consumed = true; }
    if (tb_end_x_ && tb_end_x_->handle_event(e)) { apply_text_fields(); consumed = true; }
    if (tb_end_y_ && tb_end_y_->handle_event(e)) { apply_text_fields(); consumed = true; }
    if (tb_damage_ && tb_damage_->handle_event(e)) { apply_text_fields(); consumed = true; }

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
        if (begin_attack_drag(mp)) {
            consumed = true;
        }
    } else if (e.type == SDL_MOUSEBUTTONUP && e.button.button == SDL_BUTTON_LEFT) {
        if (dragging_attack_) {
            end_attack_drag(true);
            consumed = true;
        }
    } else if (e.type == SDL_MOUSEMOTION && dragging_attack_) {
        update_attack_drag(SDL_Point{e.motion.x, e.motion.y});
        consumed = true;
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
    if (btn_prev_frame_) btn_prev_frame_->render(renderer);
    if (btn_next_frame_) btn_next_frame_->render(renderer);
    if (dd_attack_type_) dd_attack_type_->render(renderer);
    if (btn_add_remove_) btn_add_remove_->render(renderer);
    if (btn_delete_) btn_delete_->render(renderer);
    if (btn_copy_next_) btn_copy_next_->render(renderer);
    if (tb_start_x_) tb_start_x_->render(renderer);
    if (tb_start_y_) tb_start_y_->render(renderer);
    if (tb_control_x_) tb_control_x_->render(renderer);
    if (tb_control_y_) tb_control_y_->render(renderer);
    if (tb_end_x_) tb_end_x_->render(renderer);
    if (tb_end_y_) tb_end_y_->render(renderer);
    if (tb_damage_) tb_damage_->render(renderer);
    if (btn_apply_all_) btn_apply_all_->render(renderer);
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

    if (btn_prev_frame_ && btn_next_frame_) {
        int half_w = (inner_w - DMSpacing::small_gap()) / 2;
        SDL_Rect left{x + padding, cursor_y, half_w, DMButton::height()};
        SDL_Rect right{x + padding + half_w + DMSpacing::small_gap(), cursor_y, half_w, DMButton::height()};
        btn_prev_frame_->set_rect(left);
        btn_next_frame_->set_rect(right);
        cursor_y += DMButton::height() + DMSpacing::small_gap();
    }

    if (dd_attack_type_) {
        dd_attack_type_->set_rect(place_row(DMDropdown::height()));
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

    place_pair(tb_start_x_.get(), tb_start_y_.get());
    place_pair(tb_control_x_.get(), tb_control_y_.get());
    place_pair(tb_end_x_.get(), tb_end_y_.get());
    if (tb_damage_) {
        tb_damage_->set_rect(place_row(tb_damage_->height_for_width(inner_w)));
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

    const std::string current_type = current_attack_type();
    int current_type_counter = 0;
    const int selected_idx = current_attack_vector_index();
    for (const auto& vec : frame.attack.vectors) {
        bool selected = false;
        if (vec.type == current_type) {
            selected = (current_type_counter == selected_idx && selected_idx >= 0);
            ++current_type_counter;
        }
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

        auto draw_node = [&](SDL_FPoint p, bool is_selected_node) {
            const float radius = is_selected_node ? 10.0f : 8.0f;
            SDL_Color node_col = is_selected_node ? DMStyles::AccentButton().hover_bg : line_color;
            SDL_SetRenderDrawColor(renderer, node_col.r, node_col.g, node_col.b, 255);
            SDL_FRect r{p.x - radius, p.y - radius, radius * 2.0f, radius * 2.0f};
            SDL_RenderFillRectF(renderer, &r);
            SDL_SetRenderDrawColor(renderer, DMStyles::Border().r, DMStyles::Border().g, DMStyles::Border().b, 255);
            SDL_RenderDrawRectF(renderer, &r);
        };
        draw_node(start_screen, selected);
        draw_node(end_screen, selected);
        if (selected) {
            const float cr = 6.0f;
            SDL_SetRenderDrawColor(renderer, 200, 200, 200, 255);
            for (int i = 0; i < 16; ++i) {
                const float a = (static_cast<float>(i) / 16.0f) * 2.0f * static_cast<float>(M_PI);
                const float b = (static_cast<float>(i + 1) / 16.0f) * 2.0f * static_cast<float>(M_PI);
                SDL_RenderDrawLineF(renderer, control_screen.x + std::cos(a) * cr, control_screen.y + std::sin(a) * cr,
                                    control_screen.x + std::cos(b) * cr, control_screen.y + std::sin(b) * cr);
            }
        }
    }
}

void AttackGeoFrameEditor::select_frame(int index) {
    selected_index_ = clamp_index(index, static_cast<int>(frames_.size()));
    clamp_attack_selection();
    refresh_attack_form();
    refresh_selection_state();
}

void AttackGeoFrameEditor::clamp_attack_selection() {
    const std::string type = current_attack_type();
    if (frames_.empty()) {
        set_current_attack_vector_index(-1);
        return;
    }
    const int frame_index = clamp_index(selected_index_, static_cast<int>(frames_.size()));
    if (frame_index >= static_cast<int>(frames_.size())) {
        set_current_attack_vector_index(-1);
        return;
    }
    int count = 0;
    for (const auto& v : frames_[frame_index].attack.vectors) {
        if (v.type == type) {
            ++count;
        }
    }
    if (count == 0) {
        set_current_attack_vector_index(-1);
        return;
    }
    int idx = current_attack_vector_index();
    idx = std::clamp(idx, 0, count - 1);
    set_current_attack_vector_index(idx);
}

void AttackGeoFrameEditor::refresh_attack_form() const {
    const_cast<AttackGeoFrameEditor*>(this)->clamp_attack_selection();
    const auto* vec = current_attack_vector();
    if (vec) {
        if (tb_start_x_ && !tb_start_x_->is_editing()) {
            std::string text = std::to_string(static_cast<int>(std::lround(vec->start_x)));
            if (text != last_start_x_) {
                tb_start_x_->set_value(text);
                last_start_x_ = text;
            }
        }
        if (tb_start_y_ && !tb_start_y_->is_editing()) {
            std::string text = std::to_string(static_cast<int>(std::lround(vec->start_y)));
            if (text != last_start_y_) {
                tb_start_y_->set_value(text);
                last_start_y_ = text;
            }
        }
        if (tb_control_x_ && !tb_control_x_->is_editing()) {
            std::string text = std::to_string(static_cast<int>(std::lround(vec->control_x)));
            if (text != last_control_x_) {
                tb_control_x_->set_value(text);
                last_control_x_ = text;
            }
        }
        if (tb_control_y_ && !tb_control_y_->is_editing()) {
            std::string text = std::to_string(static_cast<int>(std::lround(vec->control_y)));
            if (text != last_control_y_) {
                tb_control_y_->set_value(text);
                last_control_y_ = text;
            }
        }
        if (tb_end_x_ && !tb_end_x_->is_editing()) {
            std::string text = std::to_string(static_cast<int>(std::lround(vec->end_x)));
            if (text != last_end_x_) {
                tb_end_x_->set_value(text);
                last_end_x_ = text;
            }
        }
        if (tb_end_y_ && !tb_end_y_->is_editing()) {
            std::string text = std::to_string(static_cast<int>(std::lround(vec->end_y)));
            if (text != last_end_y_) {
                tb_end_y_->set_value(text);
                last_end_y_ = text;
            }
        }
        if (tb_damage_ && !tb_damage_->is_editing()) {
            std::string text = std::to_string(vec->damage);
            if (text != last_damage_) {
                tb_damage_->set_value(text);
                last_damage_ = text;
            }
        }
        if (btn_add_remove_) btn_add_remove_->set_text("Delete Attack");
        if (btn_delete_) btn_delete_->set_text("Delete Attack");
    } else {
        if (tb_start_x_ && !tb_start_x_->is_editing()) { tb_start_x_->set_value("0"); last_start_x_ = "0"; }
        if (tb_start_y_ && !tb_start_y_->is_editing()) { tb_start_y_->set_value("0"); last_start_y_ = "0"; }
        if (tb_control_x_ && !tb_control_x_->is_editing()) { tb_control_x_->set_value("0"); last_control_x_ = "0"; }
        if (tb_control_y_ && !tb_control_y_->is_editing()) { tb_control_y_->set_value("0"); last_control_y_ = "0"; }
        if (tb_end_x_ && !tb_end_x_->is_editing()) { tb_end_x_->set_value("0"); last_end_x_ = "0"; }
        if (tb_end_y_ && !tb_end_y_->is_editing()) { tb_end_y_->set_value("0"); last_end_y_ = "0"; }
        if (tb_damage_ && !tb_damage_->is_editing()) { tb_damage_->set_value("0"); last_damage_ = "0"; }
        if (btn_add_remove_) btn_add_remove_->set_text("Add Attack");
        if (btn_delete_) btn_delete_->set_text("Delete Attack");
    }
}

void AttackGeoFrameEditor::apply_text_fields() {
    auto* vec = current_attack_vector();
    if (!vec) {
        return;
    }
    bool changed = false;
    if (tb_start_x_) {
        float v = parse_float(tb_start_x_->value(), vec->start_x);
        if (std::fabs(v - vec->start_x) > 0.001f) { vec->start_x = v; changed = true; }
        last_start_x_ = tb_start_x_->value();
    }
    if (tb_start_y_) {
        float v = parse_float(tb_start_y_->value(), vec->start_y);
        if (std::fabs(v - vec->start_y) > 0.001f) { vec->start_y = v; changed = true; }
        last_start_y_ = tb_start_y_->value();
    }
    if (tb_control_x_) {
        float v = parse_float(tb_control_x_->value(), vec->control_x);
        if (std::fabs(v - vec->control_x) > 0.001f) { vec->control_x = v; changed = true; }
        last_control_x_ = tb_control_x_->value();
    }
    if (tb_control_y_) {
        float v = parse_float(tb_control_y_->value(), vec->control_y);
        if (std::fabs(v - vec->control_y) > 0.001f) { vec->control_y = v; changed = true; }
        last_control_y_ = tb_control_y_->value();
    }
    if (tb_end_x_) {
        float v = parse_float(tb_end_x_->value(), vec->end_x);
        if (std::fabs(v - vec->end_x) > 0.001f) { vec->end_x = v; changed = true; }
        last_end_x_ = tb_end_x_->value();
    }
    if (tb_end_y_) {
        float v = parse_float(tb_end_y_->value(), vec->end_y);
        if (std::fabs(v - vec->end_y) > 0.001f) { vec->end_y = v; changed = true; }
        last_end_y_ = tb_end_y_->value();
    }
    if (tb_damage_) {
        int v = parse_int(tb_damage_->value(), vec->damage);
        if (v != vec->damage) { vec->damage = v; changed = true; }
        last_damage_ = tb_damage_->value();
    }
    if (changed) {
        persist_changes();
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

void AttackGeoFrameEditor::apply_attack_to_all_frames() {
    const auto* source = current_attack_vector();
    const std::string type = current_attack_type();
    for (auto& f : frames_) {
        auto& dest_vecs = f.attack.vectors;
        dest_vecs.erase(std::remove_if(dest_vecs.begin(), dest_vecs.end(),
                                       [&](const auto& v) { return v.type == type; }),
                        dest_vecs.end());
        if (source) {
            dest_vecs.push_back(*source);
        }
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
    const std::string type = current_attack_type();
    const auto* source = current_attack_vector();
    if (!source) return;
    auto& dest_vecs = frames_[next_index].attack.vectors;
    dest_vecs.erase(std::remove_if(dest_vecs.begin(), dest_vecs.end(),
                                   [&](const auto& v) { return v.type == type; }),
                    dest_vecs.end());
    dest_vecs.push_back(*source);
    set_current_attack_vector_index(static_cast<int>(dest_vecs.size()) - 1);
    persist_changes();
}

std::string AttackGeoFrameEditor::current_attack_type() const {
    int idx = std::clamp(selected_attack_type_index_, 0, static_cast<int>(kDamageTypeNames.size()) - 1);
    return kDamageTypeNames[static_cast<std::size_t>(idx)];
}

int AttackGeoFrameEditor::current_attack_vector_index() const {
    const int type_idx = std::clamp(selected_attack_type_index_, 0, static_cast<int>(kDamageTypeNames.size()) - 1);
    return selected_attack_vector_indices_[static_cast<std::size_t>(type_idx)];
}

void AttackGeoFrameEditor::set_current_attack_vector_index(int index) {
    const int type_idx = std::clamp(selected_attack_type_index_, 0, static_cast<int>(kDamageTypeNames.size()) - 1);
    selected_attack_vector_indices_[static_cast<std::size_t>(type_idx)] = index;
}

animation_update::FrameAttackGeometry::Vector* AttackGeoFrameEditor::current_attack_vector() {
    clamp_attack_selection();
    const int vector_index = current_attack_vector_index();
    if (frames_.empty()) return nullptr;
    const int frame_index = clamp_index(selected_index_, static_cast<int>(frames_.size()));
    auto& frame = frames_[frame_index];
    const std::string type = current_attack_type();
    int counter = 0;
    for (auto& v : frame.attack.vectors) {
        if (v.type != type) continue;
        if (counter == vector_index) {
            return &v;
        }
        ++counter;
    }
    return nullptr;
}

const animation_update::FrameAttackGeometry::Vector* AttackGeoFrameEditor::current_attack_vector() const {
    return const_cast<AttackGeoFrameEditor*>(this)->current_attack_vector();
}

animation_update::FrameAttackGeometry::Vector* AttackGeoFrameEditor::ensure_attack_vector_for_type(const std::string& type) {
    if (frames_.empty()) return nullptr;
    const int frame_index = clamp_index(selected_index_, static_cast<int>(frames_.size()));
    auto& frame = frames_[frame_index];
    animation_update::FrameAttackGeometry::Vector vec;
    vec.type = type;
    frame.attack.vectors.push_back(vec);
    int type_index = 0;
    for (const auto& v : frame.attack.vectors) {
        if (v.type == type) {
            ++type_index;
        }
    }
    set_current_attack_vector_index(type_index - 1);
    auto* added = frame.attack.vectors.empty() ? nullptr : &frame.attack.vectors.back();
    return added;
}

void AttackGeoFrameEditor::delete_current_attack_vector() {
    if (frames_.empty()) return;
    const int frame_index = clamp_index(selected_index_, static_cast<int>(frames_.size()));
    auto& frame = frames_[frame_index];
    const int index = current_attack_vector_index();
    const std::string type = current_attack_type();
    int counter = 0;
    for (auto it = frame.attack.vectors.begin(); it != frame.attack.vectors.end(); ++it) {
        if (it->type != type) continue;
        if (counter == index) {
            frame.attack.vectors.erase(it);
            break;
        }
        ++counter;
    }
    clamp_attack_selection();
    persist_changes();
}

bool AttackGeoFrameEditor::begin_attack_drag(SDL_Point mp) {
    if (!context_.assets || !context_.target || frames_.empty()) return false;

    const int frame_index = clamp_index(selected_index_, static_cast<int>(frames_.size()));
    auto& frame = frames_[frame_index];
    const std::string current_type = current_attack_type();

    const WarpedScreenGrid& cam = context_.camera ? *context_.camera : context_.assets->getView();
    SDL_Point anchor = asset_anchor_world();
    const float scale = asset_local_scale();
    auto to_screen = [&](float lx, float ly) -> SDL_FPoint {
        SDL_FPoint world{static_cast<float>(anchor.x) + lx * scale, static_cast<float>(anchor.y) - ly * scale};
        return cam.map_to_screen_f(world);
    };

    auto point_hit = [&](SDL_FPoint p, float radius) -> bool {
        const float dx = static_cast<float>(mp.x) - p.x;
        const float dy = static_cast<float>(mp.y) - p.y;
        return dx * dx + dy * dy <= radius * radius;
    };
    const float node_radius = 12.0f;

    int type_counter = 0;
    int clicked_vector_index = -1;
    AttackHandle clicked_handle = AttackHandle::None;

    for (const auto& vec : frame.attack.vectors) {
        if (vec.type != current_type) continue;

        SDL_FPoint start_screen = to_screen(vec.start_x, vec.start_y);
        SDL_FPoint control_screen = to_screen(vec.control_x, vec.control_y);
        SDL_FPoint end_screen = to_screen(vec.end_x, vec.end_y);

        if (point_hit(start_screen, node_radius)) {
            clicked_vector_index = type_counter;
            clicked_handle = AttackHandle::Start;
            break;
        } else if (point_hit(control_screen, node_radius)) {
            clicked_vector_index = type_counter;
            clicked_handle = AttackHandle::Control;
            break;
        } else if (point_hit(end_screen, node_radius)) {
            clicked_vector_index = type_counter;
            clicked_handle = AttackHandle::End;
            break;
        }
        ++type_counter;
    }

    if (clicked_vector_index < 0) {
        type_counter = 0;
        constexpr int segments = 16;
        constexpr float segment_hit_radius = 8.0f;
        for (const auto& vec : frame.attack.vectors) {
            if (vec.type != current_type) continue;
            SDL_FPoint start_screen = to_screen(vec.start_x, vec.start_y);
            SDL_FPoint control_screen = to_screen(vec.control_x, vec.control_y);
            SDL_FPoint end_screen = to_screen(vec.end_x, vec.end_y);
            for (int i = 0; i <= segments; ++i) {
                const float t = static_cast<float>(i) / static_cast<float>(segments);
                const float u = 1.0f - t;
                SDL_FPoint curve_point{
                    u * u * start_screen.x + 2.0f * u * t * control_screen.x + t * t * end_screen.x,
                    u * u * start_screen.y + 2.0f * u * t * control_screen.y + t * t * end_screen.y};
                if (point_hit(curve_point, segment_hit_radius)) {
                    clicked_vector_index = type_counter;
                    clicked_handle = AttackHandle::Segment;
                    break;
                }
            }
            if (clicked_vector_index >= 0) break;
            ++type_counter;
        }
    }

    if (clicked_vector_index < 0 || clicked_handle == AttackHandle::None) {
        return false;
    }
    set_current_attack_vector_index(clicked_vector_index);
    clamp_attack_selection();
    refresh_attack_form();
    active_handle_ = clicked_handle;

    const auto* vec = current_attack_vector();
    if (!vec) {
        active_handle_ = AttackHandle::None;
        return false;
    }
    SDL_FPoint mouse_local{};
    if (!screen_to_local(mp, mouse_local)) {
        active_handle_ = AttackHandle::None;
        return false;
    }
    dragging_attack_ = true;
    drag_moved_ = false;
    drag_start_mouse_ = mp;
    drag_start_mouse_local_ = mouse_local;
    drag_start_vector_ = *vec;
    refresh_selection_state();
    return true;
}

void AttackGeoFrameEditor::update_attack_drag(SDL_Point mouse) {
    if (!dragging_attack_) return;
    auto* vec = current_attack_vector();
    if (!vec) {
        dragging_attack_ = false;
        active_handle_ = AttackHandle::None;
        return;
    }
    SDL_FPoint local{};
    if (!screen_to_local(mouse, local)) {
        return;
    }
    const float move_threshold = 1.5f;
    if (std::fabs(local.x - drag_start_mouse_local_.x) > move_threshold ||
        std::fabs(local.y - drag_start_mouse_local_.y) > move_threshold) {
        drag_moved_ = true;
    }

    auto apply_axis = [&](SDL_FPoint& dst, const SDL_FPoint& src) {
        switch (selection_state_ ? selection_state_->axis : AdjustmentAxis::Z) {
            case AdjustmentAxis::X:
                dst.x = src.x;
                break;
            case AdjustmentAxis::Y:
                dst.y = src.y;
                break;
            case AdjustmentAxis::Z:
            default:
                dst = src;
                break;
        }
    };

    switch (active_handle_) {
        case AttackHandle::Start:
            {
                SDL_FPoint next{drag_start_vector_.start_x + (local.x - drag_start_mouse_local_.x),
                                drag_start_vector_.start_y + (local.y - drag_start_mouse_local_.y)};
                SDL_FPoint dst{vec->start_x, vec->start_y};
                apply_axis(dst, next);
                vec->start_x = dst.x;
                vec->start_y = dst.y;
            }
            break;
        case AttackHandle::Control:
            {
                SDL_FPoint next{drag_start_vector_.control_x + (local.x - drag_start_mouse_local_.x),
                                drag_start_vector_.control_y + (local.y - drag_start_mouse_local_.y)};
                SDL_FPoint dst{vec->control_x, vec->control_y};
                apply_axis(dst, next);
                vec->control_x = dst.x;
                vec->control_y = dst.y;
            }
            break;
        case AttackHandle::End:
            {
                SDL_FPoint next{drag_start_vector_.end_x + (local.x - drag_start_mouse_local_.x),
                                drag_start_vector_.end_y + (local.y - drag_start_mouse_local_.y)};
                SDL_FPoint dst{vec->end_x, vec->end_y};
                apply_axis(dst, next);
                vec->end_x = dst.x;
                vec->end_y = dst.y;
            }
            break;
        case AttackHandle::Segment: {
            SDL_FPoint delta{local.x - drag_start_mouse_local_.x, local.y - drag_start_mouse_local_.y};
            vec->start_x = drag_start_vector_.start_x + delta.x;
            vec->start_y = drag_start_vector_.start_y + delta.y;
            vec->control_x = drag_start_vector_.control_x + delta.x;
            vec->control_y = drag_start_vector_.control_y + delta.y;
            vec->end_x = drag_start_vector_.end_x + delta.x;
            vec->end_y = drag_start_vector_.end_y + delta.y;
            break;
        }
        case AttackHandle::None:
        default:
            break;
    }
    refresh_attack_form();
    persist_changes();
}

void AttackGeoFrameEditor::end_attack_drag(bool commit) {
    if (!dragging_attack_) return;
    AttackHandle handle = active_handle_;
    dragging_attack_ = false;
    active_handle_ = AttackHandle::None;
    if (!commit) {
        if (auto* vec = current_attack_vector()) {
            *vec = drag_start_vector_;
            refresh_attack_form();
        }
        return;
    }
    if (!drag_moved_ && (handle == AttackHandle::Start || handle == AttackHandle::End)) {
        delete_current_attack_vector();
    } else {
        refresh_attack_form();
        persist_changes();
    }
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

void AttackGeoFrameEditor::refresh_selection_state() {
    if (!selection_state_ || !context_.assets || !context_.target) {
        return;
    }
    const auto* vec = current_attack_vector();
    if (!vec) {
        selection_state_->target = SelectionTarget::None;
        selection_state_->attack_vector_index = -1;
        return;
    }
    selection_state_->target = SelectionTarget::AttackStart;
    selection_state_->attack_vector_index = current_attack_vector_index();
    SDL_Point anchor = asset_anchor_world();
    SDL_FPoint world{
        static_cast<float>(anchor.x) + vec->start_x * asset_local_scale(),
        static_cast<float>(anchor.y) - vec->start_y * asset_local_scale()
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
