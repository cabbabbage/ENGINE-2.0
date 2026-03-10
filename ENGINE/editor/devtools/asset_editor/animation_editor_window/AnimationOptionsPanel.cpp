#include "AnimationOptionsPanel.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "AnimationDocument.hpp"
#include "EditorUIPrimitives.hpp"
#include "PanelLayoutConstants.hpp"
#include "devtools/draw_utils.hpp"
#include "devtools/dm_styles.hpp"
#include "devtools/widgets.hpp"
#include "utils/sdl_render_conversions.hpp"
#include "utils/ttf_render_utils.hpp"

namespace {

using animation_editor::AnimationOptionsPanel;

void render_label(SDL_Renderer* renderer, const std::string& text, int x, int y) {
    if (!renderer || text.empty()) {
        return;
    }
    const DMLabelStyle& style = DMStyles::Label();
    TTF_Font* font = style.open_font();
    if (!font) {
        return;
    }

    SDL_Surface* surf = ttf_util::RenderTextBlended(font, text.c_str(), style.color);
    if (!surf) {
        TTF_CloseFont(font);
        return;
    }

    SDL_Texture* tex = SDL_CreateTextureFromSurface(renderer, surf);
    if (tex) {
        SDL_Rect dst{x, y, surf->w, surf->h};
        sdl_render::Texture(renderer, tex, nullptr, &dst);
        SDL_DestroyTexture(tex);
    }

    SDL_DestroySurface(surf);
    TTF_CloseFont(font);
}

float normalize_speed(float raw) {
    if (!std::isfinite(raw) || raw <= 0.0f) {
        return 1.0f;
    }
    return raw;
}

float parse_speed_multiplier(const nlohmann::json& payload) {
    try {
        if (payload.contains("speed_multiplier") && payload["speed_multiplier"].is_number()) {
            return normalize_speed(payload["speed_multiplier"].get<float>());
        }
        if (payload.contains("speed_factor") && payload["speed_factor"].is_number()) {
            return normalize_speed(payload["speed_factor"].get<float>());
        }
    } catch (...) {
    }
    return 1.0f;
}

const std::vector<float>& speed_options_static() {
    static const std::vector<float> options{0.25f, 0.5f, 1.0f, 2.0f, 4.0f};
    return options;
}

const std::vector<std::string>& speed_labels_static() {
    static const std::vector<std::string> labels{"0.25x", "0.5x", "1.0x", "2.0x", "4.0x"};
    return labels;
}

int find_best_speed_index(float speed) {
    const auto& options = speed_options_static();
    int best = 0;
    float best_diff = std::numeric_limits<float>::max();
    for (int idx = 0; idx < static_cast<int>(options.size()); ++idx) {
        float diff = std::fabs(options[idx] - speed);
        if (diff < best_diff) {
            best_diff = diff;
            best = idx;
        }
    }
    return best;
}

nlohmann::json parse_payload_text(const std::string& text) {
    if (text.empty()) {
        return nlohmann::json::object();
    }
    nlohmann::json parsed = nlohmann::json::parse(text, nullptr, false);
    if (!parsed.is_object()) {
        return nlohmann::json::object();
    }
    return parsed;
}

}

namespace animation_editor {

AnimationOptionsPanel::AnimationOptionsPanel() = default;

void AnimationOptionsPanel::set_document(std::shared_ptr<AnimationDocument> document) {
    if (document_ == document) {
        return;
    }
    document_ = std::move(document);
    payload_signature_.clear();
    layout_dirty_ = true;
}

void AnimationOptionsPanel::set_animation_id(const std::string& animation_id) {
    if (animation_id_ == animation_id) {
        return;
    }
    animation_id_ = animation_id;
    payload_signature_.clear();
    layout_dirty_ = true;
    sync_from_document();
}

void AnimationOptionsPanel::set_bounds(const SDL_Rect& bounds) {
    bounds_ = bounds;
    layout_dirty_ = true;
}

void AnimationOptionsPanel::update() {
    sync_from_document();
    layout_widgets();
}

void AnimationOptionsPanel::render(SDL_Renderer* renderer) const {
    if (!renderer) {
        return;
    }
    layout_widgets();

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    dm_draw::DrawBeveledRect(renderer,
                             bounds_,
                             DMStyles::CornerRadius(),
                             DMStyles::BevelDepth(),
                             DMStyles::PanelBG(),
                             DMStyles::HighlightColor(),
                             DMStyles::ShadowColor(),
                             false,
                             DMStyles::HighlightIntensity(),
                             DMStyles::ShadowIntensity());

    if (bounds_.w > 0 && bounds_.h > 0) {
        render_label(renderer, "Animation Options", label_rect_.x, label_rect_.y);
    }
    if (speed_dropdown_) speed_dropdown_->render(renderer);
}

bool AnimationOptionsPanel::handle_event(const SDL_Event& e) {
    layout_widgets();
    bool handled = false;

    if (speed_dropdown_) {
        int before = speed_dropdown_->selected();
        if (speed_dropdown_->handle_event(e)) {
            handled = true;
            if (!syncing_ && speed_dropdown_->selected() != before) {
                apply_changes_from_controls();
            }
        }
    }

    return handled;
}

int AnimationOptionsPanel::preferred_height(int width) const {
    const int inner_width = std::max(0, width - kPanelPadding * 2);
    if (inner_width <= 0) {
        return 0;
    }
    const int padding = kPanelPadding;
    const auto& label_style = DMStyles::Label();
    int label_height = label_style.font_size;
    if (label_height < 0) {
        label_height = 0;
    }
    int height = padding + label_height + DMSpacing::small_gap();
    height += DMDropdown::height();
    height += padding;
    return height;
}

void AnimationOptionsPanel::set_on_animation_properties_changed(std::function<void(const std::string&, const nlohmann::json&)> callback) {
    on_animation_properties_changed_ = std::move(callback);
}

void AnimationOptionsPanel::ensure_widgets() {
    if (!speed_dropdown_) {
        speed_dropdown_ = std::make_unique<DMDropdown>("Speed Multiplier", speed_labels_static(), 2);
    }
}

void AnimationOptionsPanel::layout_widgets() const {
    if (!layout_dirty_) {
        return;
    }
    const_cast<AnimationOptionsPanel*>(this)->ensure_widgets();
    layout_dirty_ = false;

    const int padding = kPanelPadding;
    const int width = std::max(0, bounds_.w - padding * 2);
    const int x = bounds_.x + padding;
    int y = bounds_.y + padding;
    const auto& label_style = DMStyles::Label();
    int label_height = label_style.font_size;
    if (label_height < 0) {
        label_height = 0;
    }
    label_rect_ = SDL_Rect{x, y, width, label_height};
    y += label_height + DMSpacing::small_gap();

    if (speed_dropdown_) {
        SDL_Rect rect{x, y, width, DMDropdown::height()};
        speed_dropdown_->set_rect(rect);
        y += rect.h;
    }
}

void AnimationOptionsPanel::sync_from_document() {
    if (!document_ || animation_id_.empty()) {
        return;
    }

    auto payload_text = document_->animation_payload(animation_id_);
    if (!payload_text) {
        payload_signature_.clear();
        return;
    }
    if (*payload_text == payload_signature_) {
        return;
    }

    payload_signature_ = *payload_text;
    nlohmann::json payload = parse_payload_text(*payload_text);
    cached_speed_ = parse_speed_multiplier(payload);

    ensure_widgets();
    syncing_ = true;
    if (speed_dropdown_) {
        int idx = find_best_speed_index(cached_speed_);
        speed_dropdown_->set_selected(idx);
    }
    syncing_ = false;
}

void AnimationOptionsPanel::apply_changes_from_controls() {
    if (!document_ || animation_id_.empty()) {
        return;
    }
    float desired_speed = selected_speed();
    const bool speed_changed = std::fabs(desired_speed - cached_speed_) > 1e-3f;
    if (!speed_changed) {
        return;
    }

    nlohmann::json payload = nlohmann::json::object();
    if (auto payload_text = document_->animation_payload(animation_id_)) {
        payload = parse_payload_text(*payload_text);
    }
    payload["speed_multiplier"] = desired_speed;

    document_->replace_animation_payload(animation_id_, payload.dump());

    if (auto updated = document_->animation_payload(animation_id_)) {
        payload_signature_ = *updated;
        nlohmann::json normalized = parse_payload_text(*updated);
        cached_speed_ = parse_speed_multiplier(normalized);
        if (on_animation_properties_changed_) {
            on_animation_properties_changed_(animation_id_, normalized);
        }
    } else {
        payload_signature_.clear();
    }
}

float AnimationOptionsPanel::selected_speed() const {
    if (!speed_dropdown_) {
        return cached_speed_;
    }
    const auto& options = speed_options_static();
    if (options.empty()) {
        return cached_speed_;
    }
    int idx = std::clamp(speed_dropdown_->selected(), 0, static_cast<int>(options.size()) - 1);
    return options[idx];
}
}
