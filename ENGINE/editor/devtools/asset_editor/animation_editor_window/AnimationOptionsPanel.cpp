#include "AnimationOptionsPanel.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <string>

#include "AnimationDocument.hpp"
#include "EditorUIPrimitives.hpp"
#include "PanelLayoutConstants.hpp"
#include "devtools/draw_utils.hpp"
#include "devtools/dm_styles.hpp"
#include "utils/sdl_render_conversions.hpp"
#include "utils/ttf_render_utils.hpp"

namespace {

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

} // namespace

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
    if (!renderer || bounds_.w <= 0 || bounds_.h <= 0) {
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

    render_label(renderer, "Animation Options", label_rect_.x, label_rect_.y);
}

bool AnimationOptionsPanel::handle_event(const SDL_Event& e) {
    (void)e;
    return false;
}

int AnimationOptionsPanel::preferred_height(int width) const {
    (void)width;
    return 0;
}

void AnimationOptionsPanel::set_on_animation_properties_changed(std::function<void(const std::string&, const nlohmann::json&)> callback) {
    on_animation_properties_changed_ = std::move(callback);
}

void AnimationOptionsPanel::ensure_widgets() {
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
    int label_height = std::max(0, label_style.font_size);
    label_rect_ = SDL_Rect{x, y, width, label_height};
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
    payload_signature_ = *payload_text;
}

void AnimationOptionsPanel::apply_changes_from_controls() {
}

} // namespace animation_editor
