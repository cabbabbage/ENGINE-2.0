#include "rendering/render/finale_effects.hpp"

#include <algorithm>
#include <cmath>

#include "rendering/render/render_diagnostics.hpp"

namespace {

SDL_Texture* g_previous_frame = nullptr;
bool g_previous_frame_ready = false;

std::string safe_sdl_error() {
    const char* error = SDL_GetError();
    return error ? std::string(error) : std::string{};
}

float opacity_from_percent(float percent) {
    if (!std::isfinite(percent)) {
        return 1.0f;
    }

    return std::clamp(percent / 100.0f, 0.0f, 1.0f);
}

Uint8 alpha_from_percent(float percent) {
    const float opacity = opacity_from_percent(percent);
    return static_cast<Uint8>(std::clamp(
        static_cast<int>(std::lround(opacity * 255.0f)),
        0,
        255));
}

bool texture_matches_size(SDL_Texture* texture, int width, int height) {
    if (!texture || width <= 0 || height <= 0) {
        return false;
    }

    float texture_w = 0.0f;
    float texture_h = 0.0f;
    if (!SDL_GetTextureSize(texture, &texture_w, &texture_h)) {
        return false;
    }

    return static_cast<int>(std::lround(texture_w)) == width &&
           static_cast<int>(std::lround(texture_h)) == height;
}

SDL_Texture* create_target(
    SDL_Renderer* renderer,
    int width,
    int height,
    std::string& out_error) {
    out_error.clear();

    if (!renderer || width <= 0 || height <= 0) {
        out_error = "[FinalEffects] Invalid target creation input.";
        return nullptr;
    }

    SDL_Texture* texture = render_diagnostics::create_texture(
        renderer,
        SDL_PIXELFORMAT_RGBA32,
        SDL_TEXTUREACCESS_TARGET,
        width,
        height);

    if (!texture) {
        out_error = "[FinalEffects] Failed to create target: " + safe_sdl_error();
        return nullptr;
    }

    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
    return texture;
}

bool ensure_previous_frame(
    SDL_Renderer* renderer,
    int width,
    int height,
    std::string& out_error) {
    if (texture_matches_size(g_previous_frame, width, height)) {
        return true;
    }

    render_diagnostics::destroy_texture(g_previous_frame);
    g_previous_frame = create_target(renderer, width, height, out_error);
    g_previous_frame_ready = false;
    return g_previous_frame != nullptr;
}

struct TextureState {
    SDL_BlendMode blend_mode = SDL_BLENDMODE_BLEND;
    Uint8 alpha_mod = 255;
    Uint8 color_r = 255;
    Uint8 color_g = 255;
    Uint8 color_b = 255;
};

TextureState capture_texture_state(SDL_Texture* texture) {
    TextureState state{};

    if (!texture) {
        return state;
    }

    SDL_GetTextureBlendMode(texture, &state.blend_mode);
    SDL_GetTextureAlphaMod(texture, &state.alpha_mod);
    SDL_GetTextureColorMod(texture, &state.color_r, &state.color_g, &state.color_b);

    return state;
}

void restore_texture_state(SDL_Texture* texture, const TextureState& state) {
    if (!texture) {
        return;
    }

    SDL_SetTextureBlendMode(texture, state.blend_mode);
    SDL_SetTextureAlphaMod(texture, state.alpha_mod);
    SDL_SetTextureColorMod(texture, state.color_r, state.color_g, state.color_b);
}

bool bind_target_and_clear(
    SDL_Renderer* renderer,
    SDL_Texture* target,
    SDL_Color clear_color,
    std::string& out_error,
    const char* label) {
    if (!render_diagnostics::set_render_target(renderer, target)) {
        out_error = std::string("[FinalEffects] Failed to bind ") + label + ": " + safe_sdl_error();
        return false;
    }

    SDL_SetRenderViewport(renderer, nullptr);
    SDL_SetRenderClipRect(renderer, nullptr);
    SDL_SetRenderScale(renderer, 1.0f, 1.0f);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, clear_color.r, clear_color.g, clear_color.b, clear_color.a);
    SDL_RenderClear(renderer);

    return true;
}

bool draw_full_texture(
    SDL_Renderer* renderer,
    SDL_Texture* texture,
    int width,
    int height,
    Uint8 alpha,
    std::string& out_error,
    const char* label) {
    if (!renderer || !texture || width <= 0 || height <= 0) {
        out_error = std::string("[FinalEffects] Invalid draw input for ") + label + ".";
        return false;
    }

    const TextureState state = capture_texture_state(texture);

    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    SDL_SetTextureAlphaMod(texture, alpha);
    SDL_SetTextureColorMod(texture, 255, 255, 255);

    const SDL_FRect full_rect{
        0.0f,
        0.0f,
        static_cast<float>(width),
        static_cast<float>(height)
    };

    const bool ok = render_diagnostics::render_texture(renderer, texture, nullptr, &full_rect);

    restore_texture_state(texture, state);

    if (!ok) {
        out_error = std::string("[FinalEffects] Failed to draw ") + label + ": " + safe_sdl_error();
        return false;
    }

    return true;
}

} // namespace

namespace finale_effects {

SDL_Texture* FinalEffects::apply(
    SDL_Renderer* renderer,
    SDL_Texture* source,
    SDL_Texture* destination,
    int width,
    int height,
    std::string& out_error) {
    out_error.clear();

    if (!renderer || !source || !destination || width <= 0 || height <= 0) {
        out_error = "[FinalEffects] Invalid apply input.";
        return nullptr;
    }

    SDL_Texture* current = source;

    if constexpr (FinalEffectSettings::brightness_enabled) {
        current = apply_brightness(renderer, current, destination, width, height, out_error);
        if (!current) return nullptr;
    }

    if constexpr (FinalEffectSettings::saturation_enabled) {
        current = apply_saturation(renderer, current, destination, width, height, out_error);
        if (!current) return nullptr;
    }

    if constexpr (FinalEffectSettings::contrast_enabled) {
        current = apply_contrast(renderer, current, destination, width, height, out_error);
        if (!current) return nullptr;
    }

    if constexpr (FinalEffectSettings::sharpness_enabled) {
        current = apply_sharpness(renderer, current, destination, width, height, out_error);
        if (!current) return nullptr;
    }

    if constexpr (FinalEffectSettings::vignette_enabled) {
        current = apply_vignette(renderer, current, destination, width, height, out_error);
        if (!current) return nullptr;
    }

    if constexpr (FinalEffectSettings::film_grain_enabled) {
        current = apply_film_grain(renderer, current, destination, width, height, out_error);
        if (!current) return nullptr;
    }

    if constexpr (FinalEffectSettings::chromatic_aberration_enabled) {
        current = apply_chromatic_aberration(renderer, current, destination, width, height, out_error);
        if (!current) return nullptr;
    }

    if constexpr (FinalEffectSettings::motion_blur_enabled) {
        current = apply_motion_blur(renderer, current, destination, width, height, out_error);
        if (!current) return nullptr;
    } else {
        if (current != destination &&
            !copy_texture(renderer, current, destination, width, height, out_error)) {
            return nullptr;
        }

        current = destination;
    }

    return current;
}

void FinalEffects::destroy_cached_textures() {
    render_diagnostics::destroy_texture(g_previous_frame);
    g_previous_frame_ready = false;
}

void FinalEffects::reset_motion_blur_history() {
    g_previous_frame_ready = false;
}

bool FinalEffects::copy_texture(
    SDL_Renderer* renderer,
    SDL_Texture* source,
    SDL_Texture* destination,
    int width,
    int height,
    std::string& out_error) {
    out_error.clear();

    if (!renderer || !source || !destination || width <= 0 || height <= 0) {
        out_error = "[FinalEffects] Invalid copy_texture input.";
        return false;
    }

    SDL_Texture* previous_target = SDL_GetRenderTarget(renderer);

    if (!bind_target_and_clear(
            renderer,
            destination,
            SDL_Color{0, 0, 0, 0},
            out_error,
            "copy destination")) {
        render_diagnostics::set_render_target(renderer, previous_target);
        return false;
    }

    const bool ok = draw_full_texture(renderer, source, width, height, 255, out_error, "copy source");

    render_diagnostics::set_render_target(renderer, previous_target);

    return ok;
}

SDL_Texture* FinalEffects::apply_brightness(
    SDL_Renderer* renderer,
    SDL_Texture* source,
    SDL_Texture* destination,
    int width,
    int height,
    std::string& out_error) {
    return copy_texture(renderer, source, destination, width, height, out_error) ? destination : nullptr;
}

SDL_Texture* FinalEffects::apply_saturation(
    SDL_Renderer* renderer,
    SDL_Texture* source,
    SDL_Texture* destination,
    int width,
    int height,
    std::string& out_error) {
    return copy_texture(renderer, source, destination, width, height, out_error) ? destination : nullptr;
}

SDL_Texture* FinalEffects::apply_contrast(
    SDL_Renderer* renderer,
    SDL_Texture* source,
    SDL_Texture* destination,
    int width,
    int height,
    std::string& out_error) {
    return copy_texture(renderer, source, destination, width, height, out_error) ? destination : nullptr;
}

SDL_Texture* FinalEffects::apply_sharpness(
    SDL_Renderer* renderer,
    SDL_Texture* source,
    SDL_Texture* destination,
    int width,
    int height,
    std::string& out_error) {
    return copy_texture(renderer, source, destination, width, height, out_error) ? destination : nullptr;
}

SDL_Texture* FinalEffects::apply_vignette(
    SDL_Renderer* renderer,
    SDL_Texture* source,
    SDL_Texture* destination,
    int width,
    int height,
    std::string& out_error) {
    return copy_texture(renderer, source, destination, width, height, out_error) ? destination : nullptr;
}

SDL_Texture* FinalEffects::apply_film_grain(
    SDL_Renderer* renderer,
    SDL_Texture* source,
    SDL_Texture* destination,
    int width,
    int height,
    std::string& out_error) {
    return copy_texture(renderer, source, destination, width, height, out_error) ? destination : nullptr;
}

SDL_Texture* FinalEffects::apply_chromatic_aberration(
    SDL_Renderer* renderer,
    SDL_Texture* source,
    SDL_Texture* destination,
    int width,
    int height,
    std::string& out_error) {
    return copy_texture(renderer, source, destination, width, height, out_error) ? destination : nullptr;
}

SDL_Texture* FinalEffects::apply_motion_blur(
    SDL_Renderer* renderer,
    SDL_Texture* source,
    SDL_Texture* destination,
    int width,
    int height,
    std::string& out_error) {
    out_error.clear();

    if (!renderer || !source || !destination || width <= 0 || height <= 0) {
        out_error = "[FinalEffects] Invalid motion blur input.";
        return nullptr;
    }

    if (!ensure_previous_frame(renderer, width, height, out_error)) {
        return nullptr;
    }

    SDL_Texture* previous_target = SDL_GetRenderTarget(renderer);

    const Uint8 new_frame_alpha =
        alpha_from_percent(FinalEffectSettings::motion_blur_new_frame_opacity_percent);

    // Output starts as previous frame.
    // If previous frame is not ready yet, it is black.
    if (!bind_target_and_clear(
            renderer,
            destination,
            SDL_Color{0, 0, 0, 255},
            out_error,
            "motion blur destination")) {
        render_diagnostics::set_render_target(renderer, previous_target);
        return nullptr;
    }

    if (g_previous_frame_ready) {
        if (!draw_full_texture(renderer, g_previous_frame, width, height, 255, out_error, "previous frame")) {
            render_diagnostics::set_render_target(renderer, previous_target);
            return nullptr;
        }
    }

    // New frame is drawn over previous frame at the requested opacity.
    if (!draw_full_texture(renderer, source, width, height, new_frame_alpha, out_error, "new frame")) {
        render_diagnostics::set_render_target(renderer, previous_target);
        return nullptr;
    }

    // Save merged result as the next previous frame.
    if (!copy_texture(renderer, destination, g_previous_frame, width, height, out_error)) {
        render_diagnostics::set_render_target(renderer, previous_target);
        return nullptr;
    }

    g_previous_frame_ready = true;

    render_diagnostics::set_render_target(renderer, previous_target);

    return destination;
}

} // namespace finale_effects

SDL_Texture* helperpereffects(
    SDL_Renderer* renderer,
    SDL_Texture* source,
    SDL_Texture* destination,
    int width,
    int height,
    std::string& out_error) {
    return finale_effects::FinalEffects::apply(
        renderer,
        source,
        destination,
        width,
        height,
        out_error);
}